/** @brief Authenticated, deadline-bound Linux Unix-socket witness client and service (#115). */
module;
#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
export module mddlog.adapter.unixanchorprovider;
import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditanchor;
import mddlog.adapter.witnesscodec;
import mddlog.adapter.filestoragemedium;

export namespace mddlog::adapter {
enum class WitnessIssue : std::uint8_t { InvalidConfig, Connect, Authentication, Timeout, Transport, Protocol, Denied, Unavailable, Refused, Listen };
struct WitnessError {
    WitnessIssue issue;
    int          nativeError = 0;
};
inline constexpr auto defaultWitnessTimeout = std::chrono::seconds{1};
struct UnixWitnessConfig {
    std::filesystem::path     socketPath;
    std::string               providerId;
    uid_t                     serverUid = 0;
    std::chrono::milliseconds timeout{defaultWitnessTimeout};
};
struct WitnessPermission {
    uid_t       uid;
    std::string streamId;
    bool        advance = false;
    bool        retire  = false;
};
struct WitnessServiceConfig {
    std::filesystem::path          socketPath;
    std::string                    providerId;
    std::vector<uid_t>             readers;
    std::vector<WitnessPermission> permissions;
    std::chrono::milliseconds      timeout{defaultWitnessTimeout};
};

namespace witnesstransport {
class Failure final : public std::runtime_error {
public:
    explicit Failure(WitnessError reason) : std::runtime_error("witness operation failed"), error(reason) {}
    WitnessError error;
};
using Deadline = std::chrono::steady_clock::time_point;
[[noreturn]] inline void fail(WitnessIssue issue, int nativeError = 0) {
    throw Failure({.issue = issue, .nativeError = nativeError});
}
[[nodiscard]] inline bool validConfig(const std::filesystem::path& path, std::string_view identity, std::chrono::milliseconds timeout) {
    const sockaddr_un address{};
    return path.is_absolute() && !path.native().contains('\0') && path.native().size() < sizeof(address.sun_path) && !identity.empty()
           && identity.size() <= witness::maxTextBytes && !identity.contains('\0') && timeout.count() > 0 && timeout.count() <= std::numeric_limits<int>::max();
}
[[nodiscard]] inline sockaddr_un addressOf(const std::filesystem::path& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::ranges::copy(path.native(), std::span{address.sun_path}.begin());
    return address;
}
inline void ready(int descriptor, std::int16_t events, Deadline deadline) {
    while (true) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0)
            fail(WitnessIssue::Timeout);
        pollfd    descriptorState{.fd = descriptor, .events = events, .revents = 0};
        const int result = ::poll(&descriptorState, 1, static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining, std::numeric_limits<int>::max())));
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            fail(WitnessIssue::Transport, errno);
        if (result == 0)
            fail(WitnessIssue::Timeout);
        if ((static_cast<unsigned>(descriptorState.revents) & static_cast<unsigned>(events)) != 0)
            return;
        fail(WitnessIssue::Transport);
    }
}
inline void send(int descriptor, std::span<const std::uint8_t> bytes, Deadline deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        ready(descriptor, POLLOUT, deadline);
        const auto remaining = bytes.subspan(offset);
        const auto wrote     = ::send(descriptor, remaining.data(), remaining.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (wrote < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (wrote <= 0)
            fail(WitnessIssue::Transport, wrote < 0 ? errno : 0);
        offset += static_cast<std::size_t>(wrote);
    }
}
inline void receive(int descriptor, std::span<std::uint8_t> bytes, Deadline deadline) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        ready(descriptor, POLLIN, deadline);
        const auto remaining = bytes.subspan(offset);
        // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection): MSG_DONTWAIT guarantees nonblocking recv; poll applies the remaining transport deadline.
        const auto got = ::recv(descriptor, remaining.data(), remaining.size(), MSG_DONTWAIT);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (got <= 0)
            fail(WitnessIssue::Transport, got < 0 ? errno : 0);
        offset += static_cast<std::size_t>(got);
    }
}
inline void sendFrame(int descriptor, std::span<const std::uint8_t> bytes, Deadline deadline) {
    witness::Encoder header;
    header.number(bytes.size());
    send(descriptor, header.bytes, deadline);
    send(descriptor, bytes, deadline);
}
[[nodiscard]] inline std::vector<std::uint8_t> receiveFrame(int descriptor, Deadline deadline) {
    std::array<std::uint8_t, sizeof(std::uint64_t)> header{};
    receive(descriptor, header, deadline);
    witness::Decoder decoder{header};
    const auto       size = decoder.number();
    if (size == 0 || size > witness::maxBytes)
        fail(WitnessIssue::Protocol);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    receive(descriptor, bytes, deadline);
    return bytes;
}
[[nodiscard]] inline uid_t peer(int descriptor) {
    ucred     credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || size != sizeof(credentials))
        fail(WitnessIssue::Authentication, errno);
    return credentials.uid;
}
[[nodiscard]] inline detail::FileDescriptor connect(const UnixWitnessConfig& config, Deadline deadline) {
    detail::FileDescriptor socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socket.get() < 0)
        fail(WitnessIssue::Connect, errno);
    const auto address = addressOf(config.socketPath);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket address ABI.
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        // AF_UNIX EAGAIN (full backlog) does not establish a pending connection.
        if (errno != EINPROGRESS)
            fail(WitnessIssue::Connect, errno);
        ready(socket.get(), POLLOUT, deadline);
        int       error = 0;
        socklen_t size  = sizeof(error);
        if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &size) != 0)
            fail(WitnessIssue::Connect, errno);
        if (error != 0)
            fail(WitnessIssue::Connect, error);
    }
    if (peer(socket.get()) != config.serverUid)
        fail(WitnessIssue::Authentication);
    return socket;
}
}  // namespace witnesstransport

/**
 * @brief AnchorProvider client with centrally pinned service UID and enrolled provider identity.
 * Each call uses one connection and a single total transport deadline. Missing, denied, malformed or
 * unauthenticated responses return ProviderUnavailable; lastError distinguishes the cause without
 * exposing access material. A mutation may have committed before a missing response: reconcile using
 * latest/streams. This adapter never treats a missing reply as acceptance or retries a mutation silently.
 */
class UnixAnchorProvider final : public AnchorProvider {
public:
    explicit UnixAnchorProvider(UnixWitnessConfig settings) : config(std::move(settings)) {}
    [[nodiscard]] std::optional<WitnessError> lastError() const {
        const std::scoped_lock guard(mutex);
        return error;
    }
    [[nodiscard]] AdvanceAnswer advance(const AnchorClaim& claim) override {
        const std::scoped_lock guard(mutex);
        if (claim.anchorFormat == 0 || claim.canonicalVersion == 0 || claim.position == 0 || !core::AuditEvent::validStreamId(claim.streamId))
            return refused(AdvanceRefusal::Malformed);
        return invoke<AdvanceAnswer>(
            witness::Operation::Advance,
            [&](auto& request) {
                request.number(claim.anchorFormat);
                request.number(claim.canonicalVersion);
                request.text(claim.streamId);
                request.number(claim.position);
                request.raw(claim.digest);
            },
            [&](auto status, auto& response) -> AdvanceAnswer {
                if (status == witness::Reply::Accepted)
                    return stamp(response);
                if (status == witness::Reply::PositionNotIncreasing)
                    return refused(AdvanceRefusal::PositionNotIncreasing);
                if (status == witness::Reply::Conflict)
                    return refused(AdvanceRefusal::Conflict);
                if (status == witness::Reply::Malformed)
                    return refused(AdvanceRefusal::Malformed);
                witnesstransport::fail(WitnessIssue::Protocol);
            });
    }
    [[nodiscard]] RetireAnswer retire(std::string_view streamId, std::uint64_t position) override {
        const std::scoped_lock guard(mutex);
        return invoke<RetireAnswer>(
            witness::Operation::Retire,
            [&](auto& request) {
                request.text(streamId);
                request.number(position);
            },
            [&](auto status, auto& response) -> RetireAnswer {
                if (status == witness::Reply::Accepted)
                    return stamp(response);
                if (status == witness::Reply::Conflict)
                    return refused(RetireRefusal::Conflict);
                if (status == witness::Reply::UnknownStream)
                    return refused(RetireRefusal::UnknownStream);
                witnesstransport::fail(WitnessIssue::Protocol);
            });
    }
    [[nodiscard]] LatestAnswer latest(std::string_view streamId) override {
        const std::scoped_lock guard(mutex);
        return invoke<LatestAnswer>(
            witness::Operation::Latest,
            [&](auto& request) {
                request.text(streamId);
            },
            [&](auto status, auto& response) -> LatestAnswer {
                if (status != witness::Reply::Listing)
                    witnesstransport::fail(WitnessIssue::Protocol);
                auto listing = response.listing();
                if (!witness::validListing(listing, config.providerId))
                    witnesstransport::fail(WitnessIssue::Protocol);
                // latest carries the authenticated full head so all invariants can be checked before selecting a stream.
                for (const auto& entry : listing.entries)
                    if (witness::anchorOf(entry).streamId == streamId)
                        return std::visit(
                            [](const auto& value) -> LatestAnswer {
                                return value;
                            },
                            entry);
                return AnchorAbsent{};
            });
    }
    [[nodiscard]] StreamsAnswer streams() override {
        const std::scoped_lock guard(mutex);
        return invoke<StreamsAnswer>(
            witness::Operation::Streams,
            [](auto&) {},
            [&](auto status, auto& response) -> StreamsAnswer {
                if (status != witness::Reply::Listing)
                    witnesstransport::fail(WitnessIssue::Protocol);
                auto listing = response.listing();
                if (!witness::validListing(listing, config.providerId))
                    witnesstransport::fail(WitnessIssue::Protocol);
                return listing;
            });
    }

private:
    template <class Refusal>
    [[nodiscard]] Refusal refused(Refusal reason) {
        error = WitnessError{.issue = WitnessIssue::Refused};
        return reason;
    }
    [[nodiscard]] AnchorStamp stamp(witness::Decoder& response) const {
        AnchorStamp value{.providerId = response.text(), .counter = response.number(), .acceptedTime = response.time()};
        if (value.providerId != config.providerId || value.counter == 0)
            witnesstransport::fail(WitnessIssue::Protocol);
        return value;
    }
    template <class Answer, class Write, class Read>
    [[nodiscard]] Answer invoke(witness::Operation operation, Write write, Read read) {
        error.reset();
        try {
            if (!witnesstransport::validConfig(config.socketPath, config.providerId, config.timeout))
                witnesstransport::fail(WitnessIssue::InvalidConfig);
            witness::Encoder request;
            request.number(witness::protocolVersion);
            request.number(static_cast<std::uint64_t>(operation));
            request.text(config.providerId);
            write(request);
            const auto deadline = std::chrono::steady_clock::now() + config.timeout;
            auto       socket   = witnesstransport::connect(config, deadline);
            witnesstransport::sendFrame(socket.get(), request.bytes, deadline);
            const auto       bytes = witnesstransport::receiveFrame(socket.get(), deadline);
            witness::Decoder response{bytes};
            if (response.number() != witness::protocolVersion)
                witnesstransport::fail(WitnessIssue::Protocol);
            const auto status = response.reply();
            if (status == witness::Reply::Denied)
                witnesstransport::fail(WitnessIssue::Denied);
            if (status == witness::Reply::Unavailable)
                witnesstransport::fail(WitnessIssue::Unavailable);
            auto answer = read(status, response);
            response.end();
            return answer;
        } catch (const witnesstransport::Failure& failed) {
            error = failed.error;
        } catch (const std::invalid_argument&) {
            error = WitnessError{.issue = WitnessIssue::Protocol};
        } catch (const std::length_error&) {
            error = WitnessError{.issue = WitnessIssue::Protocol};
        }
        return ProviderUnavailable{};
    }
    UnixWitnessConfig           config;
    std::optional<WitnessError> error;
    mutable std::mutex          mutex;
};

/**
 * @brief Single-consumer witness endpoint. The host owns the durable authority and its policy.
 * The socket parent must be service-owned and unwritable by group/others. No existing socket is
 * unlinked automatically. Provisioning/removal is an administrator action after the old service exits.
 * serveOnce uses a bounded idle wait and bounded connection transfers; persistence itself remains
 * synchronous. Mutation permissions are exact stream/UID matches, reads require an enrolled UID.
 */
class UnixWitnessService {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<UnixWitnessService>, WitnessError> create(WitnessServiceConfig config, AnchorProvider& authority) {
        if (!witnesstransport::validConfig(config.socketPath, config.providerId, config.timeout) || config.readers.empty()
            || config.permissions.size() > witness::maxStreams || !std::ranges::all_of(config.permissions, [](const auto& permission) {
                   return core::AuditEvent::validStreamId(permission.streamId);
               }))
            return std::unexpected(WitnessError{.issue = WitnessIssue::InvalidConfig});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open uses fixed flags.
        detail::FileDescriptor parent(::open(config.socketPath.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat            info{};
        if (parent.get() < 0 || ::fstat(parent.get(), &info) != 0)
            return std::unexpected(WitnessError{.issue = WitnessIssue::Listen, .nativeError = errno});
        if (info.st_uid != ::geteuid() || (info.st_mode & (mode_t{S_IWGRP} | mode_t{S_IWOTH})) != 0)
            return std::unexpected(WitnessError{.issue = WitnessIssue::InvalidConfig});
        const auto  listing = authority.streams();
        const auto* state   = std::get_if<ProviderListing>(&listing);
        if (state == nullptr || !witness::validListing(*state, config.providerId))
            return std::unexpected(WitnessError{.issue = WitnessIssue::Unavailable});
        detail::FileDescriptor listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (listener.get() < 0)
            return std::unexpected(WitnessError{.issue = WitnessIssue::Listen, .nativeError = errno});
        const auto address = witnesstransport::addressOf(config.socketPath);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket address ABI.
        if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            return std::unexpected(WitnessError{.issue = WitnessIssue::Listen, .nativeError = errno});
        // bind applies the umask first; chmod only grants connection access, with SO_PEERCRED enforcing the policy.
        constexpr mode_t socketMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        if (::chmod(config.socketPath.c_str(), socketMode) != 0 || ::listen(listener.get(), SOMAXCONN) != 0)
            return std::unexpected(WitnessError{.issue = WitnessIssue::Listen, .nativeError = errno});
        return std::unique_ptr<UnixWitnessService>(
            new UnixWitnessService(std::move(config),
                                   authority,
                                   std::move(listener)));  // NOLINT(cppcoreguidelines-owning-memory): private constructor, immediately owned by unique_ptr.
    }
    /** @brief True if one connection was processed, false on idle timeout. Call only from the service consumer. */
    [[nodiscard]] std::expected<bool, WitnessError> serveOnce() {
        try {
            try {
                witnesstransport::ready(listener.get(), POLLIN, std::chrono::steady_clock::now() + config.timeout);
            } catch (const witnesstransport::Failure& failed) {
                if (failed.error.issue == WitnessIssue::Timeout)
                    return false;
                throw;
            }
            detail::FileDescriptor socket(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (socket.get() < 0)
                witnesstransport::fail(WitnessIssue::Transport, errno);
            const auto deadline = std::chrono::steady_clock::now() + config.timeout;
            const auto uid      = witnesstransport::peer(socket.get());
            if (std::ranges::find(config.readers, uid) == config.readers.end() && !std::ranges::any_of(config.permissions, [uid](const auto& permission) {
                    return permission.uid == uid;
                })) {
                witness::Encoder denied;
                denied.number(witness::protocolVersion);
                reply(denied, witness::Reply::Denied);
                witnesstransport::sendFrame(socket.get(), denied.bytes, deadline);
                return true;
            }
            const auto       bytes = witnesstransport::receiveFrame(socket.get(), deadline);
            witness::Decoder request{bytes};
            if (request.number() != witness::protocolVersion)
                witnesstransport::fail(WitnessIssue::Protocol);
            const auto operation = request.operation();
            if (request.text() != config.providerId)
                witnesstransport::fail(WitnessIssue::Authentication);
            witness::Encoder response;
            response.number(witness::protocolVersion);
            dispatch(uid, operation, request, response);
            witnesstransport::sendFrame(socket.get(), response.bytes, deadline);
            return true;
        } catch (const witnesstransport::Failure& failed) {
            return std::unexpected(failed.error);
        } catch (const std::invalid_argument&) {
            return std::unexpected(WitnessError{.issue = WitnessIssue::Protocol});
        } catch (const std::length_error&) {
            return std::unexpected(WitnessError{.issue = WitnessIssue::Protocol});
        }
    }

private:
    UnixWitnessService(WitnessServiceConfig settings, AnchorProvider& provider, detail::FileDescriptor socket)
        : config(std::move(settings)), authority(&provider), listener(std::move(socket)) {}
    [[nodiscard]] bool permitted(uid_t uid, std::string_view stream, bool retire) const {
        return std::ranges::any_of(config.permissions, [&](const auto& permission) {
            return permission.uid == uid && permission.streamId == stream && (retire ? permission.retire : permission.advance);
        });
    }
    static void accepted(witness::Encoder& response, const AnchorStamp& stamp) {
        response.number(static_cast<std::uint64_t>(witness::Reply::Accepted));
        response.text(stamp.providerId);
        response.number(stamp.counter);
        response.time(stamp.acceptedTime);
    }
    static void reply(witness::Encoder& response, witness::Reply status) {
        response.number(static_cast<std::uint64_t>(status));
    }
    void dispatch(uid_t uid, witness::Operation operation, witness::Decoder& request, witness::Encoder& response) {
        if (operation == witness::Operation::Advance) {
            AnchorClaim claim{.anchorFormat     = request.version(),
                              .canonicalVersion = request.version(),
                              .streamId         = request.text(),
                              .position         = request.number()};
            std::ranges::copy(request.raw(sha256DigestSize), claim.digest.begin());
            request.end();
            if (!permitted(uid, claim.streamId, false)) {
                reply(response, witness::Reply::Denied);
                return;
            }
            const auto answer = authority->advance(claim);
            if (const auto* stamp = std::get_if<AnchorStamp>(&answer))
                accepted(response, *stamp);
            else if (const auto* refusal = std::get_if<AdvanceRefusal>(&answer)) {
                switch (*refusal) {
                    case AdvanceRefusal::PositionNotIncreasing:
                        reply(response, witness::Reply::PositionNotIncreasing);
                        break;
                    case AdvanceRefusal::Conflict:
                        reply(response, witness::Reply::Conflict);
                        break;
                    case AdvanceRefusal::Malformed:
                        reply(response, witness::Reply::Malformed);
                        break;
                }
            } else
                reply(response, witness::Reply::Unavailable);
        } else if (operation == witness::Operation::Retire) {
            const auto stream   = request.text();
            const auto position = request.number();
            request.end();
            if (!permitted(uid, stream, true)) {
                reply(response, witness::Reply::Denied);
                return;
            }
            const auto answer = authority->retire(stream, position);
            if (const auto* stamp = std::get_if<AnchorStamp>(&answer))
                accepted(response, *stamp);
            else if (const auto* refusal = std::get_if<RetireRefusal>(&answer))
                reply(response, *refusal == RetireRefusal::Conflict ? witness::Reply::Conflict : witness::Reply::UnknownStream);
            else
                reply(response, witness::Reply::Unavailable);
        } else if (operation == witness::Operation::Latest || operation == witness::Operation::Streams) {
            if (operation == witness::Operation::Latest)
                (void)request.text();
            request.end();
            if (std::ranges::find(config.readers, uid) == config.readers.end()) {
                reply(response, witness::Reply::Denied);
                return;
            }
            const auto answer = authority->streams();
            if (const auto* listing = std::get_if<ProviderListing>(&answer)) {
                reply(response, witness::Reply::Listing);
                response.listing(*listing);
            } else
                reply(response, witness::Reply::Unavailable);
        } else
            witnesstransport::fail(WitnessIssue::Protocol);
    }
    WitnessServiceConfig   config;
    AnchorProvider*        authority;
    detail::FileDescriptor listener;
};
}  // namespace mddlog::adapter
