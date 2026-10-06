/** @brief Durable anchor authority owned only by the witness service, with fail-closed restoration (#115). */
module;
#include <cerrno>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
export module mddlog.adapter.fileanchorauthority;
import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditanchor;
import mddlog.adapter.witnesscodec;
import mddlog.adapter.filestoragemedium;

export namespace mddlog::adapter {
enum class WitnessStoreIssue : std::uint8_t { Missing, Invalid, IdentityMismatch, Access, Busy, Read, Write, Sync, Stopped, Capacity };
struct WitnessStoreError {
    WitnessStoreIssue issue;
    int               nativeError = 0;
};
struct WitnessStoreConfig {
    std::filesystem::path directory;
    std::string           providerId;
};

/**
 * @brief Service-private state: explicit enrolment, exclusive lifetime lock, durable-before-acknowledgement mutations.
 * After a write or barrier failure every operation becomes unavailable until the authority is reopened.
 * Recovery validates the entire state and synchronizes it before exposing it; a lost response must be
 * reconciled by latest/streams, never by weakening advance/retire conflict refusals.
 */
class FileAnchorAuthority final : public AnchorProvider {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<FileAnchorAuthority>, WitnessStoreError> open(WitnessStoreConfig config,
                                                                                                     FileStorageCalls*  calls = nullptr) {
        auto made = acquire(std::move(config), calls);
        if (!made)
            return made;
        auto& store = **made;
        if (!store.restore())
            return std::unexpected(store.error.value_or(WitnessStoreError{.issue = WitnessStoreIssue::Stopped}));
        // Reconfirm a visible rename from a previous uncertain transaction before serving it.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX openat has fixed flags.
        detail::FileDescriptor file(::openat(store.directory.get(), "witness.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (file.get() < 0)
            return std::unexpected(store.fail(WitnessStoreIssue::Read, errno));
        if (!store.sync(file.get()) || !store.sync(store.directory.get()))
            return std::unexpected(store.fail(WitnessStoreIssue::Sync, errno));
        return made;
    }
    /** @brief Explicit administrator enrolment. Existing or damaged state is never overwritten. */
    [[nodiscard]] static std::expected<void, WitnessStoreError> initialize(WitnessStoreConfig config, FileStorageCalls* calls = nullptr) {
        auto made = acquire(std::move(config), calls);
        if (!made)
            return std::unexpected(made.error());
        auto&       store = **made;
        struct stat info{};
        if (::fstatat(store.directory.get(), "witness.bin", &info, AT_SYMLINK_NOFOLLOW) == 0)
            return std::unexpected(WitnessStoreError{.issue = WitnessStoreIssue::Invalid});
        if (errno != ENOENT)
            return std::unexpected(store.fail(WitnessStoreIssue::Access, errno));
        if (!store.persist(store.state))
            return std::unexpected(store.error.value_or(WitnessStoreError{.issue = WitnessStoreIssue::Stopped}));
        return {};
    }
    /** @brief Latched storage/restoration failure; a pre-write capacity refusal does not set this error. */
    [[nodiscard]] std::optional<WitnessStoreError> lastError() const {
        const std::scoped_lock guard(mutex);
        return error;
    }
    [[nodiscard]] AdvanceAnswer advance(const AnchorClaim& claim) override {
        const std::scoped_lock guard(mutex);
        if (error)
            return ProviderUnavailable{};
        if (claim.anchorFormat == 0 || claim.canonicalVersion == 0 || claim.position == 0 || !core::AuditEvent::validStreamId(claim.streamId))
            return AdvanceRefusal::Malformed;
        auto next  = state;
        auto found = std::ranges::find_if(next.entries, [&](const auto& entry) {
            return witness::anchorOf(entry).streamId == claim.streamId;
        });
        if (found != next.entries.end()) {
            const auto& held = witness::anchorOf(*found);
            if (std::holds_alternative<Retirement>(*found))
                return claim.position <= held.position ? AdvanceRefusal::PositionNotIncreasing : AdvanceRefusal::Conflict;
            if (claim.position == held.position && claim.digest != held.digest)
                return AdvanceRefusal::Conflict;
            if (claim.position <= held.position)
                return AdvanceRefusal::PositionNotIncreasing;
        } else if (next.entries.size() == witness::maxStreams) {
            return ProviderUnavailable{};
        }
        if (next.head == std::numeric_limits<std::uint64_t>::max())
            return ProviderUnavailable{};
        AnchorStamp stamp{.providerId = next.providerId, .counter = ++next.head, .acceptedTime = now()};
        Anchor      held{.anchorFormat     = claim.anchorFormat,
                         .canonicalVersion = claim.canonicalVersion,
                         .streamId         = claim.streamId,
                         .position         = claim.position,
                         .digest           = claim.digest,
                         .providerId       = stamp.providerId,
                         .counter          = stamp.counter,
                         .acceptedTime     = stamp.acceptedTime};
        if (found == next.entries.end())
            next.entries.emplace_back(std::move(held));
        else
            *found = std::move(held);
        if (!persist(next))
            return ProviderUnavailable{};
        state = std::move(next);
        return stamp;
    }
    [[nodiscard]] RetireAnswer retire(std::string_view streamId, std::uint64_t position) override {
        const std::scoped_lock guard(mutex);
        if (error)
            return ProviderUnavailable{};
        auto       next  = state;
        const auto found = std::ranges::find_if(next.entries, [&](const auto& entry) {
            return witness::anchorOf(entry).streamId == streamId;
        });
        if (found == next.entries.end())
            return RetireRefusal::UnknownStream;
        if (std::holds_alternative<Retirement>(*found) || witness::anchorOf(*found).position != position)
            return RetireRefusal::Conflict;
        if (next.head == std::numeric_limits<std::uint64_t>::max())
            return ProviderUnavailable{};
        AnchorStamp stamp{.providerId = next.providerId, .counter = ++next.head, .acceptedTime = now()};
        *found = Retirement{.finalAnchor = std::get<Anchor>(*found), .counter = stamp.counter, .retiredTime = stamp.acceptedTime};
        if (!persist(next))
            return ProviderUnavailable{};
        state = std::move(next);
        return stamp;
    }
    [[nodiscard]] LatestAnswer latest(std::string_view streamId) override {
        const std::scoped_lock guard(mutex);
        if (error)
            return ProviderUnavailable{};
        for (const auto& entry : state.entries)
            if (witness::anchorOf(entry).streamId == streamId)
                return std::visit(
                    [](const auto& value) -> LatestAnswer {
                        return value;
                    },
                    entry);
        return AnchorAbsent{};
    }
    [[nodiscard]] StreamsAnswer streams() override {
        const std::scoped_lock guard(mutex);
        return error ? StreamsAnswer{ProviderUnavailable{}} : StreamsAnswer{state};
    }

private:
    FileAnchorAuthority(WitnessStoreConfig config, detail::FileDescriptor root, FileStorageCalls* systemCalls)
        : identity(std::move(config.providerId)),
          directory(std::move(root)),
          calls(systemCalls != nullptr ? systemCalls : &defaultCalls),
          state{.providerId = identity, .head = 0, .entries = {}} {}
    [[nodiscard]] static std::expected<std::unique_ptr<FileAnchorAuthority>, WitnessStoreError> acquire(WitnessStoreConfig config, FileStorageCalls* calls) {
        if (config.directory.empty() || config.directory.native().contains('\0') || config.providerId.empty()
            || config.providerId.size() > witness::maxTextBytes || config.providerId.contains('\0'))
            return std::unexpected(WitnessStoreError{.issue = WitnessStoreIssue::Invalid});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open uses fixed flags.
        detail::FileDescriptor root(::open(config.directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat            info{};
        if (root.get() < 0 || ::fstat(root.get(), &info) != 0)
            return std::unexpected(WitnessStoreError{.issue = WitnessStoreIssue::Access, .nativeError = errno});
        if (info.st_uid != ::geteuid() || (info.st_mode & (mode_t{S_IRWXG} | mode_t{S_IRWXO})) != 0)
            return std::unexpected(WitnessStoreError{.issue = WitnessStoreIssue::Access});
        if (::flock(root.get(), LOCK_EX | LOCK_NB) != 0)
            return std::unexpected(WitnessStoreError{.issue = WitnessStoreIssue::Busy, .nativeError = errno});
        return std::unique_ptr<FileAnchorAuthority>(
            new FileAnchorAuthority(std::move(config),
                                    std::move(root),
                                    calls));  // NOLINT(cppcoreguidelines-owning-memory): private constructor; ownership immediately enters unique_ptr.
    }
    [[nodiscard]] WitnessStoreError fail(WitnessStoreIssue issue, int nativeError = 0) {
        error = WitnessStoreError{.issue = issue, .nativeError = nativeError};
        return *error;
    }
    [[nodiscard]] static core::RawTime now() {
        return core::RawTime::available(std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now()));
    }
    [[nodiscard]] bool sync(int descriptor) {
        while (true) {
            const int result = calls->sync(descriptor);
            if (result >= 0 || errno != EINTR)
                return result == 0;
        }
    }
    [[nodiscard]] bool restore() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX openat uses fixed flags.
        detail::FileDescriptor file(::openat(directory.get(), "witness.bin", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        if (file.get() < 0) {
            (void)fail(errno == ENOENT ? WitnessStoreIssue::Missing : WitnessStoreIssue::Read, errno);
            return false;
        }
        struct stat info{};
        if (::fstat(file.get(), &info) != 0) {
            (void)fail(WitnessStoreIssue::Read, errno);
            return false;
        }
        if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & (mode_t{S_IRWXG} | mode_t{S_IRWXO})) != 0 || info.st_nlink != 1) {
            (void)fail(WitnessStoreIssue::Access);
            return false;
        }
        if (info.st_size < 0 || std::cmp_greater(info.st_size, witness::maxBytes) || std::cmp_less(info.st_size, sha256DigestSize)) {
            (void)fail(WitnessStoreIssue::Invalid);
            return false;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
        std::size_t               offset = 0;
        while (offset < bytes.size()) {
            const auto got = calls->read(file.get(), std::span{bytes}.subspan(offset), offset);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0 || std::cmp_greater(got, bytes.size() - offset)) {
                (void)fail(WitnessStoreIssue::Read, got < 0 ? errno : 0);
                return false;
            }
            offset += static_cast<std::size_t>(got);
        }
        const auto payload = std::span{bytes}.first(bytes.size() - sha256DigestSize);
        if (!std::ranges::equal(sha256(payload), std::span{bytes}.last(sha256DigestSize))) {
            (void)fail(WitnessStoreIssue::Invalid);
            return false;
        }
        try {
            witness::Decoder decoder{payload};
            if (decoder.text() != "mddlog-witness" || decoder.number() != witness::protocolVersion) {
                (void)fail(WitnessStoreIssue::Invalid);
                return false;
            }
            auto restored = decoder.listing();
            decoder.end();
            if (restored.providerId != identity) {
                (void)fail(WitnessStoreIssue::IdentityMismatch);
                return false;
            }
            if (!witness::validListing(restored, identity)) {
                (void)fail(WitnessStoreIssue::Invalid);
                return false;
            }
            state = std::move(restored);
            return true;
        } catch (const std::invalid_argument&) {
            (void)fail(WitnessStoreIssue::Invalid);
            return false;
        }
    }
    [[nodiscard]] bool persist(const ProviderListing& next) {
        witness::Encoder encoder;
        try {
            encoder.text("mddlog-witness");
            encoder.number(witness::protocolVersion);
            encoder.listing(next);
            const auto checksum = sha256(encoder.bytes);
            encoder.raw(checksum);
        } catch (const std::length_error&) {
            // No filesystem operation has begun; capacity refusal leaves the authority healthy.
            return false;
        }
        const int root = directory.get();
        if (::unlinkat(root, "witness.tmp", 0) != 0 && errno != ENOENT) {
            (void)fail(WitnessStoreIssue::Write, errno);
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX openat uses fixed flags and a private mode.
        detail::FileDescriptor file(::openat(root, "witness.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR));
        if (file.get() < 0) {
            (void)fail(WitnessStoreIssue::Write, errno);
            return false;
        }
        std::size_t offset = 0;
        while (offset < encoder.bytes.size()) {
            const auto wrote = calls->write(file.get(), std::span{encoder.bytes}.subspan(offset));
            if (wrote < 0 && errno == EINTR)
                continue;
            if (wrote <= 0 || std::cmp_greater(wrote, encoder.bytes.size() - offset)) {
                (void)fail(WitnessStoreIssue::Write, wrote < 0 ? errno : 0);
                return false;
            }
            offset += static_cast<std::size_t>(wrote);
        }
        if (!sync(file.get())) {
            (void)fail(WitnessStoreIssue::Sync, errno);
            return false;
        }
        if (calls->replaceMetadata(root, "witness.tmp", "witness.bin") != 0) {
            (void)fail(WitnessStoreIssue::Write, errno);
            return false;
        }
        if (!sync(root)) {
            (void)fail(WitnessStoreIssue::Sync, errno);
            return false;
        }
        return true;
    }
    std::string                      identity;
    detail::FileDescriptor           directory;
    FileStorageCalls                 defaultCalls;
    FileStorageCalls*                calls;
    ProviderListing                  state;
    std::optional<WitnessStoreError> error;
    mutable std::mutex               mutex;
};
}  // namespace mddlog::adapter
