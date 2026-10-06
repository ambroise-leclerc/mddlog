/** @brief Reference Linux witness executable with explicit enrolment and separate UID authorities (#115). */
#include <csignal>

#include <unistd.h>
import std;
import mddlog.adapter.fileanchorauthority;
import mddlog.adapter.unixanchorprovider;

namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): async signal handlers need a sig_atomic_t flag.
volatile std::sig_atomic_t stopRequested = 0;
void                       requestStop(int) {
    stopRequested = 1;
}
[[nodiscard]] uid_t parseUid(std::string_view text) {
    uid_t      value  = 0;
    const auto parsed = std::from_chars(text.begin(), text.end(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.end())
        throw std::invalid_argument("UID");
    return value;
}
}  // namespace
int main(int argc, const char* argv[]) {  // NOLINT(bugprone-exception-escape,cppcoreguidelines-pro-bounds-array-to-pointer-decay): exceptions are reported
                                          // below; main is the platform entry point.
    using namespace mddlog::adapter;
    try {
        const std::span args{argv, static_cast<std::size_t>(argc)};
        if (argc == 4 && std::string_view(args[1]) == "init") {
            const auto initialized = FileAnchorAuthority::initialize({.directory = args[2], .providerId = args[3]});
            if (!initialized) {
                std::cerr << "witness initialization failed: " << static_cast<int>(initialized.error().issue) << '\n';
                return 2;
            }
            return 0;
        }
        if (argc < 9 || std::string_view(args[1]) != "serve") {
            std::cerr << "usage: witness init DIRECTORY PROVIDER\n"
                         "       witness serve DIRECTORY SOCKET PROVIDER WRITER_UID RETIRE_UID READER_UID STREAM...\n";
            return 1;
        }
        const auto writer  = parseUid(args[5]);
        const auto retirer = parseUid(args[6]);
        const auto reader  = parseUid(args[7]);
        if (writer == ::geteuid() || retirer == ::geteuid() || reader == ::geteuid()) {
            std::cerr << "witness UID must differ from writer, retirer and reader\n";
            return 2;
        }
        auto authority = FileAnchorAuthority::open({.directory = args[2], .providerId = args[4]});
        if (!authority) {
            std::cerr << "witness restoration failed: " << static_cast<int>(authority.error().issue) << '\n';
            return 2;
        }
        WitnessServiceConfig config{
            .socketPath  = args[3],
            .providerId  = args[4],
            .readers     = {writer, retirer, reader},
            .permissions = {}
        };
        for (const auto* stream : args.subspan(8)) {
            config.permissions.push_back({.uid = writer, .streamId = stream, .advance = true});
            config.permissions.push_back({.uid = retirer, .streamId = stream, .retire = true});
        }
        auto service = UnixWitnessService::create(std::move(config), **authority);
        if (!service) {
            std::cerr << "witness listener failed: " << static_cast<int>(service.error().issue) << '\n';
            return 2;
        }
        (void)std::signal(SIGTERM, requestStop);
        (void)std::signal(SIGINT, requestStop);
        std::cout << "ready\n" << std::flush;
        while (stopRequested == 0) {
            const auto served = (*service)->serveOnce();
            if (!served)
                std::cerr << "witness request failed: " << static_cast<int>(served.error().issue) << '\n';
            if ((*authority)->lastError())
                return 3;
        }
        return 0;
    } catch (const std::exception&) {
        std::cerr << "invalid witness configuration\n";
        return 1;
    }
}
