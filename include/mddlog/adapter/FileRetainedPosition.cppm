/** @brief Atomic, provider-bound reader checkpoints in a private Linux directory (#115). */
module;
#include <cerrno>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

export module mddlog.adapter.fileretainedposition;
import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditverifier;
import mddlog.adapter.filestoragemedium;

export namespace mddlog::adapter {

enum class RetainedFileIssue : std::uint8_t { Missing, Invalid, UnsupportedVersion, ProviderMismatch, Conflict, Regression, Access, Busy, Read, Write, Sync };
struct RetainedFileError {
    RetainedFileIssue issue;
    int               nativeError = 0;
};
struct RetainedSnapshot {
    std::uint64_t    generation = 0;
    RetainedPosition position;
};

/**
 * @brief Reader-owned checkpoint store. Provision a private, durable directory outside the writer's authority.
 *
 * One store binds all stream checkpoints to one enrolled provider. load() never turns a missing or
 * damaged file into an empty position. initialize() is an explicit first-use decision; save() requires
 * the generation returned by load() and rejects stale readers and regressions. After any save failure,
 * reload and reconcile before retrying: a rename may have succeeded before a directory barrier failed.
 * All access must cooperate with the advisory lock. No protection against the reader's own authority
 * restoring its entire directory is claimed. Successful fsync requires host qualification of the medium.
 */
class FileRetainedPosition {
public:
    explicit FileRetainedPosition(std::filesystem::path root, std::string identity, FileStorageCalls* systemCalls = nullptr)
        : directory(std::move(root)), providerId(std::move(identity)), calls(systemCalls != nullptr ? systemCalls : &defaultCalls) {}
    FileRetainedPosition(const FileRetainedPosition&)            = delete;
    FileRetainedPosition& operator=(const FileRetainedPosition&) = delete;
    FileRetainedPosition(FileRetainedPosition&&)                 = delete;
    FileRetainedPosition& operator=(FileRetainedPosition&&)      = delete;
    ~FileRetainedPosition()                                      = default;

    [[nodiscard]] std::expected<RetainedSnapshot, RetainedFileError> load() {
        const std::scoped_lock guard(mutex);
        auto                   access = lock();
        if (!access)
            return std::unexpected(access.error());
        return read(access->directory.get());
    }
    /** @brief Enrol a new store explicitly. Never call automatically after a failed restoration. */
    [[nodiscard]] std::expected<std::uint64_t, RetainedFileError> initialize(const RetainedPosition& position) {
        return persist(position, 0, true);
    }
    [[nodiscard]] std::expected<std::uint64_t, RetainedFileError> save(const RetainedPosition& position, std::uint64_t expectedGeneration) {
        return persist(position, expectedGeneration, false);
    }

private:
    static constexpr std::size_t                 maxBytes         = std::size_t{1024} * 1024;
    static constexpr std::size_t                 maxEntries       = 4096;
    static constexpr std::size_t                 maxIdentityBytes = 1024;
    static constexpr std::uint64_t               formatVersion    = 1;
    static constexpr unsigned                    byteBits         = 8;
    static constexpr mode_t                      privateFileMode  = S_IRUSR | S_IWUSR;
    static constexpr mode_t                      otherPermissions = S_IRWXG | S_IRWXO;
    static constexpr std::array<std::uint8_t, 8> magic{'M', 'D', 'D', 'R', 'E', 'T', 'P', 0};
    struct Access {
        detail::FileDescriptor directory;
        detail::FileDescriptor lock;
    };
    [[nodiscard]] static auto failure(RetainedFileIssue issue, int nativeError = errno) {
        return std::unexpected(RetainedFileError{.issue = issue, .nativeError = nativeError});
    }
    [[nodiscard]] static bool privateRegular(int descriptor) {
        struct stat info{};
        if (::fstat(descriptor, &info) != 0)
            return false;
        if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & otherPermissions) != 0 || info.st_nlink != 1) {
            errno = EACCES;
            return false;
        }
        return true;
    }
    [[nodiscard]] std::expected<Access, RetainedFileError> lock() {
        if (directory.empty() || directory.native().contains('\0') || providerId.empty() || providerId.size() > maxIdentityBytes || providerId.contains('\0'))
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open uses fixed flags.
        detail::FileDescriptor root(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat            info{};
        if (root.get() < 0 || ::fstat(root.get(), &info) != 0)
            return failure(RetainedFileIssue::Access);
        if (info.st_uid != ::geteuid() || (info.st_mode & otherPermissions) != 0)
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Access});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX openat uses fixed flags and a private mode.
        detail::FileDescriptor held(::openat(root.get(), "retained.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, privateFileMode));
        if (held.get() < 0 || !privateRegular(held.get()))
            return failure(RetainedFileIssue::Access);
        if (::flock(held.get(), LOCK_EX | LOCK_NB) != 0)
            return failure(RetainedFileIssue::Busy);
        return Access{.directory = std::move(root), .lock = std::move(held)};
    }
    static void appendNumber(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
        for (unsigned i = 0; i < sizeof(value); ++i) {
            bytes.push_back(static_cast<std::uint8_t>(value));
            value >>= byteBits;
        }
    }
    static void appendText(std::vector<std::uint8_t>& bytes, std::string_view value) {
        appendNumber(bytes, value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    struct Decoder {
        std::span<const std::uint8_t> bytes;
        std::size_t                   offset = 0;
        [[nodiscard]] std::uint64_t   number() {
            if (bytes.size() - offset < sizeof(std::uint64_t))
                throw std::invalid_argument("truncated checkpoint");
            std::uint64_t value = 0;
            for (unsigned i = 0; i < sizeof(value); ++i)
                value |= std::uint64_t{bytes[offset++]} << (byteBits * i);
            return value;
        }
        [[nodiscard]] std::string text() {
            const auto size = number();
            if (size == 0 || size > maxIdentityBytes || size > bytes.size() - offset)
                throw std::invalid_argument("invalid identity");
            std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
            offset += static_cast<std::size_t>(size);
            if (value.contains('\0'))
                throw std::invalid_argument("invalid identity");
            return value;
        }
    };
    [[nodiscard]] bool valid(const RetainedPosition& position) const {
        const auto head = position.head(providerId);
        if (position.allHeads().size() != 1 || !head || position.allAnchors().size() > maxEntries)
            return false;
        return std::ranges::all_of(position.allAnchors(), [&](const auto& entry) {
            const auto& [id, anchor] = entry;
            return core::AuditEvent::validStreamId(id) && anchor.position != 0 && anchor.counter != 0 && anchor.counter <= head.value_or(0);
        });
    }
    [[nodiscard]] std::expected<RetainedSnapshot, RetainedFileError> decode(std::span<const std::uint8_t> bytes) const {
        if (bytes.size() < magic.size() + sha256DigestSize || !std::ranges::equal(bytes.first(magic.size()), magic))
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        const auto payload = bytes.first(bytes.size() - sha256DigestSize);
        if (!std::ranges::equal(sha256(payload), bytes.last(sha256DigestSize)))
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        try {
            Decoder decoder{.bytes = payload, .offset = magic.size()};
            if (decoder.number() != formatVersion)
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::UnsupportedVersion});
            RetainedSnapshot out{.generation = decoder.number(), .position = {}};
            if (out.generation == 0)
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
            if (decoder.text() != providerId)
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::ProviderMismatch});
            out.position.raiseHead(providerId, decoder.number());
            const auto count = decoder.number();
            if (count > maxEntries)
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto     id = decoder.text();
                RetainedAnchor anchor{.position = decoder.number(), .counter = decoder.number()};
                const auto     retired = decoder.number();
                if (retired > 1 || out.position.anchor(id) || decoder.bytes.size() - decoder.offset < sha256DigestSize)
                    return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
                anchor.retired = retired != 0;
                std::ranges::copy(decoder.bytes.subspan(decoder.offset, sha256DigestSize), anchor.digest.begin());
                decoder.offset += sha256DigestSize;
                out.position.raiseAnchor(id, anchor);
            }
            if (decoder.offset != payload.size() || !valid(out.position))
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
            return out;
        } catch (const std::invalid_argument&) {
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        }
    }
    [[nodiscard]] std::expected<RetainedSnapshot, RetainedFileError> read(int root) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX openat uses fixed flags.
        detail::FileDescriptor file(::openat(root, "retained.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (file.get() < 0)
            return failure(errno == ENOENT ? RetainedFileIssue::Missing : RetainedFileIssue::Read);
        if (!privateRegular(file.get()))
            return failure(RetainedFileIssue::Access);
        struct stat info{};
        if (::fstat(file.get(), &info) != 0)
            return failure(RetainedFileIssue::Read);
        if (info.st_size < 0 || std::cmp_greater(info.st_size, maxBytes))
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
        std::size_t               offset = 0;
        while (offset < bytes.size()) {
            const auto got = calls->read(file.get(), std::span{bytes}.subspan(offset), offset);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0 || static_cast<std::size_t>(got) > bytes.size() - offset)
                return failure(RetainedFileIssue::Read, got < 0 ? errno : 0);
            offset += static_cast<std::size_t>(got);
        }
        return decode(bytes);
    }
    [[nodiscard]] static bool preserves(const RetainedPosition& next, const RetainedPosition& previous) {
        for (const auto& [id, head] : previous.allHeads())
            if (next.head(id).value_or(0) < head)
                return false;
        return std::ranges::all_of(previous.allAnchors(), [&](const auto& entry) {
            const auto& [id, anchor] = entry;
            const auto value         = next.anchor(id);
            return value && value->position >= anchor.position && value->counter >= anchor.counter && (!anchor.retired || *value == anchor)
                   && (value->position != anchor.position || value->digest == anchor.digest);
        });
    }
    [[nodiscard]] bool sync(int descriptor) {
        while (true) {
            const int result = calls->sync(descriptor);
            if (result >= 0 || errno != EINTR)
                return result == 0;
        }
    }
    [[nodiscard]] std::expected<std::uint64_t, RetainedFileError> persist(const RetainedPosition& position, std::uint64_t generation, bool enrol) {
        const std::scoped_lock guard(mutex);
        auto                   access = lock();
        if (!access)
            return std::unexpected(access.error());
        if (!valid(position))
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        const auto previous = read(access->directory.get());
        if (enrol) {
            if (previous)
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Conflict});
            if (previous.error().issue != RetainedFileIssue::Missing)
                return std::unexpected(previous.error());
        } else {
            if (!previous)
                return std::unexpected(previous.error());
            if (previous->generation != generation)
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Conflict});
            if (!preserves(position, previous->position))
                return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Regression});
        }
        if (generation == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        std::vector<std::uint8_t> bytes(magic.begin(), magic.end());
        appendNumber(bytes, formatVersion);
        appendNumber(bytes, generation + 1);
        appendText(bytes, providerId);
        appendNumber(bytes, position.head(providerId).value_or(0));
        appendNumber(bytes, position.allAnchors().size());
        for (const auto& [id, anchor] : position.allAnchors()) {
            appendText(bytes, id);
            appendNumber(bytes, anchor.position);
            appendNumber(bytes, anchor.counter);
            appendNumber(bytes, anchor.retired ? 1 : 0);
            bytes.insert(bytes.end(), anchor.digest.begin(), anchor.digest.end());
        }
        const auto checksum = sha256(bytes);
        bytes.insert(bytes.end(), checksum.begin(), checksum.end());
        if (bytes.size() > maxBytes)
            return std::unexpected(RetainedFileError{.issue = RetainedFileIssue::Invalid});
        const int root = access->directory.get();
        // A leftover temporary is never a checkpoint and may be removed while holding the lock.
        if (::unlinkat(root, "retained.tmp", 0) != 0 && errno != ENOENT)
            return failure(RetainedFileIssue::Write);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX openat uses fixed flags and a private mode.
        detail::FileDescriptor file(::openat(root, "retained.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, privateFileMode));
        if (file.get() < 0)
            return failure(RetainedFileIssue::Write);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto wrote = calls->write(file.get(), std::span{bytes}.subspan(offset));
            if (wrote < 0 && errno == EINTR)
                continue;
            if (wrote <= 0 || static_cast<std::size_t>(wrote) > bytes.size() - offset)
                return failure(RetainedFileIssue::Write, wrote < 0 ? errno : 0);
            offset += static_cast<std::size_t>(wrote);
        }
        if (!sync(file.get()))
            return failure(RetainedFileIssue::Sync);
        if (calls->replaceMetadata(root, "retained.tmp", "retained.bin") != 0)
            return failure(RetainedFileIssue::Write);
        if (!sync(root))
            return failure(RetainedFileIssue::Sync);
        return generation + 1;
    }
    std::filesystem::path directory;
    std::string           providerId;
    FileStorageCalls      defaultCalls;
    FileStorageCalls*     calls;
    std::mutex            mutex;
};
}  // namespace mddlog::adapter
