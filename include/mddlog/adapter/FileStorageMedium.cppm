/** @brief Linux file storage for ADR-004, with explicit deployment eligibility and fail-closed mutations. Adapter zone only. */
module;

#include <cerrno>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

export module mddlog.adapter.filestoragemedium;

import std;
export import mddlog.adapter.auditmedium;

export namespace mddlog::adapter {

/** @brief An integrator declaration, never inferred from a filesystem name or successful write. */
enum class FileDurability : std::uint8_t { Unqualified, QualifiedFsync };
enum class FileAccess : std::uint8_t { ReadWrite, ReadOnly };
enum class FileStorageIssue : std::uint8_t { InvalidConfig, Directory, Busy, Inventory, Capacity, ReadOnly, Unavailable, Write, Read, Sync, Reclaim, Stopped };

/** @brief Last failure, including the Linux errno when a system call failed. Successful calls do not clear it. */
struct FileStorageError {
    FileStorageIssue issue       = FileStorageIssue::Unavailable;
    int              nativeError = 0;
};

/** @brief Centralized backend limits; provision and persist the private directory before opening it. */
struct FileStorageConfig {
    std::filesystem::path directory;
    std::size_t           maxSegments     = 0;
    std::uint64_t         maxSegmentBytes = 0;
    std::uint64_t         maxReadBytes    = 0;
    FileAccess            access          = FileAccess::ReadWrite;
    FileDurability        durability      = FileDurability::Unqualified;
};

/**
 * @brief System-call seam for deterministic tests. Production uses the default implementation.
 * Overrides obey write/pread/fsync return and errno conventions and outlive the medium.
 */
class FileStorageCalls {
public:
    FileStorageCalls()                                   = default;
    FileStorageCalls(const FileStorageCalls&)            = delete;
    FileStorageCalls& operator=(const FileStorageCalls&) = delete;
    FileStorageCalls(FileStorageCalls&&)                 = delete;
    FileStorageCalls& operator=(FileStorageCalls&&)      = delete;
    virtual ~FileStorageCalls()                          = default;

    [[nodiscard]] virtual std::ptrdiff_t write(int descriptor, std::span<const std::uint8_t> bytes) {
        return ::write(descriptor, bytes.data(), bytes.size());
    }
    [[nodiscard]] virtual std::ptrdiff_t read(int descriptor, std::span<std::uint8_t> bytes, std::uint64_t offset) {
        return ::pread(descriptor, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    }
    [[nodiscard]] virtual int sync(int descriptor) {
        return ::fsync(descriptor);
    }
};

namespace detail {
/** @brief Linux close is never retried: even on EINTR the descriptor has been released. */
class FileDescriptor {
public:
    explicit FileDescriptor(int initial = -1) noexcept : value(initial) {}
    FileDescriptor(const FileDescriptor&)            = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : value(std::exchange(other.value, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            value = std::exchange(other.value, -1);
        }
        return *this;
    }
    ~FileDescriptor() {
        reset();
    }
    [[nodiscard]] int get() const noexcept {
        return value;
    }

private:
    void reset() noexcept {
        if (value >= 0)
            (void)::close(value);
        value = -1;
    }
    int value;
};
}  // namespace detail

/**
 * @brief Append-only segment files in an exclusively owned Linux directory, called by one consumer thread.
 *
 * Read-only instances take a shared directory lock; a writer takes an exclusive lock. Locks are advisory:
 * all access must cooperate. Names encode opaque SegmentRef values, never caller stream IDs. Content
 * validation and recovery remain in AuditStore. Failed writes, barriers or removal stop all mutations
 * until the object is destroyed and recovery is performed; no damaged prefix is repaired here.
 *
 * @note QualifiedFsync requires a separately qualified local filesystem/device/cache deployment.
 * The destructor releases resources and never confirms durability. See docs/file-storage.md.
 */
class FileStorageMedium final : public StorageMedium {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<FileStorageMedium>, FileStorageError> create(FileStorageConfig config,
                                                                                                    FileStorageCalls* systemCalls = nullptr) {
        if (config.directory.empty() || config.directory.native().find('\0') != std::string::npos || config.maxSegments == 0 || config.maxSegmentBytes == 0
            || config.maxReadBytes == 0 || config.maxSegmentBytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())
            || config.maxReadBytes > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())
            || (config.access != FileAccess::ReadWrite && config.access != FileAccess::ReadOnly)
            || (config.durability != FileDurability::Unqualified && config.durability != FileDurability::QualifiedFsync))
            return std::unexpected(FileStorageError{.issue = FileStorageIssue::InvalidConfig});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): Linux open/openat require this variadic API; flags and mode are fixed and typed.
        detail::FileDescriptor directory(::open(config.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory.get() < 0)
            return std::unexpected(FileStorageError{.issue = FileStorageIssue::Directory, .nativeError = errno});
        struct stat metadata{};
        if (::fstat(directory.get(), &metadata) != 0)
            return std::unexpected(FileStorageError{.issue = FileStorageIssue::Directory, .nativeError = errno});
        if (metadata.st_uid != ::geteuid() || (metadata.st_mode & privateMask) != 0)
            return std::unexpected(FileStorageError{.issue = FileStorageIssue::Directory});
        const int lock = config.access == FileAccess::ReadOnly ? LOCK_SH | LOCK_NB : LOCK_EX | LOCK_NB;
        if (::flock(directory.get(), lock) != 0)
            return std::unexpected(FileStorageError{.issue = FileStorageIssue::Busy, .nativeError = errno});
        auto       medium  = std::unique_ptr<FileStorageMedium>(new FileStorageMedium(std::move(config), std::move(directory), systemCalls));
        const auto listing = medium->segments();
        if (!listing)
            return std::unexpected(medium->lastError().value_or(FileStorageError{.issue = FileStorageIssue::Inventory}));
        for (const auto& segment : *listing)
            medium->lastRef = std::max(medium->lastRef, segment.segment);
        return medium;
    }

    /** @brief Latched last error. Access only on the medium's consumer thread. */
    [[nodiscard]] std::optional<FileStorageError> lastError() const noexcept {
        return error;
    }
    [[nodiscard]] bool mutationsStopped() const noexcept {
        return stopped;
    }

    [[nodiscard]] OpenAnswer open(const SegmentOpening& opening) override {
        if (!writable())
            return {};
        const auto listing = segments();
        if (!listing)
            return {};
        if (listing->size() >= config.maxSegments || opening.bytes.size() > config.maxSegmentBytes) {
            fail(FileStorageIssue::Capacity);
            return {.status = OpenStatus::NoSpace};
        }
        if (lastRef == std::numeric_limits<SegmentRef>::max()) {
            fail(FileStorageIssue::Capacity);
            return {.status = OpenStatus::NoSpace};
        }
        const SegmentRef ref  = ++lastRef;
        const auto       name = fileName(ref);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): Linux open/openat require this variadic API; flags and mode are fixed and typed.
        detail::FileDescriptor descriptor(::openat(directory.get(), name.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, fileMode));
        if (descriptor.get() < 0) {
            const int code = errno;
            fail(FileStorageIssue::Write, code);
            return {.status = noSpace(code) ? OpenStatus::NoSpace : OpenStatus::Failed};
        }
        auto [found, inserted] = held.try_emplace(ref, std::move(descriptor));
        (void)inserted;
        if (!writeAll(found->second.get(), opening.bytes)) {
            stopped = true;
            return {.status = noSpace(error.value_or(FileStorageError{}).nativeError) ? OpenStatus::NoSpace : OpenStatus::Failed};
        }
        return {.status = OpenStatus::Opened, .segment = ref, .end = opening.bytes.size()};
    }

    [[nodiscard]] AppendAnswer append(SegmentRef segment, std::span<const std::uint8_t> bytes) override {
        if (!writable())
            return {};
        const int  descriptor = segmentDescriptor(segment);
        const auto size       = sizeOf(descriptor);
        if (!size)
            return {};
        if (bytes.size() > config.maxSegmentBytes - *size) {
            fail(FileStorageIssue::Capacity);
            return {};
        }
        if (!writeAll(descriptor, bytes)) {
            stopped = true;
            return {};
        }
        return {.status = AppendStatus::Written, .end = *size + bytes.size()};
    }

    [[nodiscard]] SyncAnswer sync(SegmentRef segment, std::uint64_t offset) override {
        if (!writable())
            return SyncAnswer::Failed;
        if (config.durability == FileDurability::Unqualified)
            return SyncAnswer::Unsupported;
        const int  descriptor = segmentDescriptor(segment);
        const auto size       = sizeOf(descriptor);
        if (!size)
            return SyncAnswer::Failed;
        if (offset > *size) {
            fail(FileStorageIssue::Sync);
            return SyncAnswer::Failed;
        }
        if (!barrier(descriptor) || !barrier(directory.get())) {
            stopped = true;
            return SyncAnswer::Failed;
        }
        return SyncAnswer::Durable;
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(SegmentRef segment, std::uint64_t offset, std::uint64_t length) override {
        if (length > config.maxReadBytes) {
            fail(FileStorageIssue::Capacity);
            return std::nullopt;
        }
        const int  descriptor = segmentDescriptor(segment);
        const auto size       = sizeOf(descriptor);
        if (!size)
            return std::nullopt;
        if (offset >= *size)
            return std::vector<std::uint8_t>{};
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min(length, *size - offset)));
        std::size_t               done = 0;
        while (done < bytes.size()) {
            const auto count = calls->read(descriptor, std::span(bytes).subspan(done), offset + done);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0 || static_cast<std::size_t>(count) > bytes.size() - done) {
                fail(FileStorageIssue::Read, count < 0 ? errno : EIO);
                return std::nullopt;
            }
            done += static_cast<std::size_t>(count);
        }
        return bytes;
    }

    [[nodiscard]] std::optional<std::vector<SegmentInfo>> segments() override {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): Linux open/openat require this variadic API; flags and mode are fixed and typed.
        const int scan = ::openat(directory.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (scan < 0) {
            fail(FileStorageIssue::Inventory, errno);
            return std::nullopt;
        }
        std::unique_ptr<DIR, decltype(&::closedir)> entries(::fdopendir(scan), &::closedir);
        if (!entries) {
            const int code = errno;
            (void)::close(scan);
            fail(FileStorageIssue::Inventory, code);
            return std::nullopt;
        }
        std::vector<SegmentInfo> listing;
        for (;;) {
            errno = 0;
            // NOLINTNEXTLINE(concurrency-mt-unsafe): this DIR belongs only to this call on the single consumer thread.
            const auto* entry = ::readdir(entries.get());
            if (entry == nullptr) {
                if (errno != 0) {
                    fail(FileStorageIssue::Inventory, errno);
                    return std::nullopt;
                }
                break;
            }
            const std::string_view name(std::span(entry->d_name).data());
            if (name == "." || name == "..")
                continue;
            const auto  ref = parseName(name);
            struct stat metadata{};
            if (!ref || ::fstatat(directory.get(), name.data(), &metadata, AT_SYMLINK_NOFOLLOW) != 0 || !validFile(metadata)
                || listing.size() >= config.maxSegments) {
                fail(FileStorageIssue::Inventory, errno);
                return std::nullopt;
            }
            listing.push_back({*ref, static_cast<std::uint64_t>(metadata.st_size)});
        }
        std::ranges::sort(listing, {}, &SegmentInfo::segment);
        return listing;
    }

    [[nodiscard]] bool reclaim(SegmentRef segment) override {
        if (!writable() || segment == 0)
            return false;
        // Validate before unlinking; never follow or remove a foreign object.
        const int descriptor = segmentDescriptor(segment);
        if (!sizeOf(descriptor))
            return false;
        const auto name = fileName(segment);
        if (::unlinkat(directory.get(), name.c_str(), 0) != 0) {
            fail(FileStorageIssue::Reclaim, errno);
            stopped = true;
            return false;
        }
        held.erase(segment);
        if (!barrier(directory.get())) {
            stopped = true;
            return false;
        }
        return true;
    }

private:
    static constexpr mode_t           privateMask = 0077;
    static constexpr mode_t           fileMode    = 0600;
    static constexpr std::size_t      nameDigits  = 16;
    static constexpr std::string_view suffix      = ".mdl";

    FileStorageMedium(FileStorageConfig declared, detail::FileDescriptor owned, FileStorageCalls* systemCalls)
        : config(std::move(declared)), directory(std::move(owned)), calls(systemCalls != nullptr ? systemCalls : &defaultCalls) {}

    void fail(FileStorageIssue issue, int code = 0) noexcept {
        error = FileStorageError{.issue = issue, .nativeError = code};
        if (issue == FileStorageIssue::Inventory)
            stopped = true;
    }
    [[nodiscard]] static bool noSpace(int code) noexcept {
        return code == ENOSPC || code == EDQUOT;
    }
    [[nodiscard]] bool writable() {
        if (stopped) {
            // Preserve the original failure while refusing all subsequent mutations.
            return false;
        }
        if (config.access == FileAccess::ReadOnly) {
            fail(FileStorageIssue::ReadOnly);
            return false;
        }
        return true;
    }
    [[nodiscard]] bool validFile(const struct stat& metadata) const noexcept {
        return S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 && metadata.st_uid == ::geteuid() && (metadata.st_mode & privateMask) == 0
               && metadata.st_size >= 0 && std::cmp_less_equal(metadata.st_size, config.maxSegmentBytes);
    }
    [[nodiscard]] static std::string fileName(SegmentRef ref) {
        return std::format("{:016x}{}", ref, suffix);
    }
    [[nodiscard]] static std::optional<SegmentRef> parseName(std::string_view name) {
        if (name.size() != nameDigits + suffix.size() || !name.ends_with(suffix))
            return std::nullopt;
        SegmentRef ref         = 0;
        const auto [end, code] = std::from_chars(name.data(), std::to_address(std::span(name).first(nameDigits).end()), ref, 16);
        if (code != std::errc{} || end != std::to_address(std::span(name).first(nameDigits).end()) || ref == 0 || fileName(ref) != name)
            return std::nullopt;
        return ref;
    }
    [[nodiscard]] int segmentDescriptor(SegmentRef ref) {
        if (ref == 0) {
            fail(FileStorageIssue::Unavailable);
            return -1;
        }
        if (const auto found = held.find(ref); found != held.end())
            return found->second.get();
        const auto name   = fileName(ref);
        const int  access = config.access == FileAccess::ReadOnly ? O_RDONLY : O_RDWR | O_APPEND;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): Linux open/openat require this variadic API; flags and mode are fixed and typed.
        detail::FileDescriptor descriptor(::openat(directory.get(), name.c_str(), access));
        if (descriptor.get() < 0) {
            fail(FileStorageIssue::Unavailable, errno);
            return -1;
        }
        if (!sizeOf(descriptor.get()))
            return -1;
        if (held.size() >= config.maxSegments) {
            fail(FileStorageIssue::Capacity);
            return -1;
        }
        const auto [found, inserted] = held.try_emplace(ref, std::move(descriptor));
        (void)inserted;
        return found->second.get();
    }
    [[nodiscard]] std::optional<std::uint64_t> sizeOf(int descriptor) {
        if (descriptor < 0)
            return std::nullopt;
        struct stat metadata{};
        if (::fstat(descriptor, &metadata) != 0) {
            fail(FileStorageIssue::Unavailable, errno);
            return std::nullopt;
        }
        if (!validFile(metadata)) {
            fail(FileStorageIssue::Inventory);
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(metadata.st_size);
    }
    [[nodiscard]] bool writeAll(int descriptor, std::span<const std::uint8_t> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto remaining = bytes.subspan(done, std::min(bytes.size() - done, static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())));
            const auto count     = calls->write(descriptor, remaining);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0 || static_cast<std::size_t>(count) > remaining.size()) {
                fail(FileStorageIssue::Write, count < 0 ? errno : EIO);
                return false;
            }
            done += static_cast<std::size_t>(count);
        }
        return true;
    }
    [[nodiscard]] bool barrier(int descriptor) {
        int result = calls->sync(descriptor);
        while (result < 0 && errno == EINTR)
            result = calls->sync(descriptor);
        if (result != 0) {
            fail(FileStorageIssue::Sync, errno);
            return false;
        }
        return true;
    }

    FileStorageConfig                            config;
    detail::FileDescriptor                       directory;
    FileStorageCalls                             defaultCalls;
    FileStorageCalls*                            calls;
    std::map<SegmentRef, detail::FileDescriptor> held;
    SegmentRef                                   lastRef = 0;
    std::optional<FileStorageError>              error;
    bool                                         stopped = false;
};

}  // namespace mddlog::adapter
