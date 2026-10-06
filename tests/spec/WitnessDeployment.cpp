/** @brief Separate-UID/process witness, writer and retained reader integration in an isolated user namespace (#115). */
#include <cerrno>
#include <csignal>
#include <cstdlib>

#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
import std;
import mddlog.adapter.fileanchorauthority;
import mddlog.adapter.unixanchorprovider;
import mddlog.adapter.fileretainedposition;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.auditservice;
import mddlog.core.auditbinding;

namespace {
using namespace mddlog::adapter;
constexpr uid_t            witnessUid   = 1;
constexpr uid_t            writerUid    = 2;
constexpr uid_t            retireUid    = 3;
constexpr uid_t            readerUid    = 4;
constexpr std::string_view providerName = "deployment-witness";
void                       require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string{message});
}
void drop(uid_t uid) {
    if (::setgroups(0, nullptr) != 0 && errno != EPERM)
        throw std::runtime_error("setgroups");
    require(::setgid(uid) == 0 && ::setuid(uid) == 0, "drop UID");
}
void child(uid_t uid, const auto& action) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork");
    if (pid == 0) {
        try {
            drop(uid);
            action();
            ::_exit(0);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            ::_exit(1);
        }
    }
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child failed");
}
struct Profile {
    Profile() {
        std::string pattern = "/tmp/mddlog-witness-profile-XXXXXX";
        const char* made    = ::mkdtemp(pattern.data());
        require(made != nullptr, "directory");
        root = made;
        require(::chmod(root.c_str(), S_IRWXU | S_IXGRP | S_IXOTH) == 0, "parent mode");
        provision("witness", witnessUid, S_IRWXU);
        provision("endpoint", witnessUid, S_IRWXU | S_IXGRP | S_IXOTH);
        provision("journal", writerUid, S_IRWXU);
        provision("reader", readerUid, S_IRWXU);
        provision("image", readerUid, S_IRWXU);
        std::filesystem::create_directory(root / "backup");
        std::filesystem::permissions(root / "backup", std::filesystem::perms::owner_all);
    }
    Profile(const Profile&)            = delete;
    Profile& operator=(const Profile&) = delete;
    Profile(Profile&&)                 = delete;
    Profile& operator=(Profile&&)      = delete;
    ~Profile() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void provision(std::string_view name, uid_t uid, mode_t mode) const {
        const auto path = root / name;
        std::filesystem::create_directory(path);
        require(::chown(path.c_str(), uid, uid) == 0 && ::chmod(path.c_str(), mode) == 0, "provision authority");
    }
    [[nodiscard]] WitnessStoreConfig authority() const {
        return {.directory = root / "witness", .providerId = std::string{providerName}};
    }
    [[nodiscard]] UnixWitnessConfig client() const {
        return {.socketPath = root / "endpoint/socket", .providerId = std::string{providerName}, .serverUid = witnessUid};
    }
    std::filesystem::path root;
};
struct Service {
    Service(const Profile& deployment, std::filesystem::path executable) : profile(deployment), binary(std::move(executable)) {
        start();
    }
    Service(const Service&)            = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&)                 = delete;
    Service& operator=(Service&&)      = delete;
    ~Service() {
        if (pid > 0) {
            (void)::kill(pid, SIGKILL);
            (void)::waitpid(pid, nullptr, 0);
        }
    }
    void start() {
        std::filesystem::remove(profile.root / "endpoint/socket");
        pid = ::fork();
        require(pid >= 0, "service fork");
        if (pid == 0) {
            try {
                drop(witnessUid);
                std::vector<std::string> args{binary.string(),
                                              "serve",
                                              (profile.root / "witness").string(),
                                              (profile.root / "endpoint/socket").string(),
                                              std::string{providerName},
                                              "2",
                                              "3",
                                              "4",
                                              "device/boot-1",
                                              "device/boot-2",
                                              "ledger/boot-1",
                                              "ledger/boot-2",
                                              "auxiliary",
                                              "concurrent"};
                std::vector<char*>       pointers;
                pointers.reserve(args.size() + 1);
                for (auto& arg : args)
                    pointers.push_back(arg.data());
                pointers.push_back(nullptr);
                (void)::execv(binary.c_str(), pointers.data());
                ::_exit(1);
            } catch (const std::exception&) {
                ::_exit(1);
            }
        }
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (std::filesystem::exists(profile.root / "endpoint/socket")) {
                try {
                    auto connected = witnesstransport::connect(profile.client(), std::chrono::steady_clock::now() + std::chrono::milliseconds{20});
                    return;
                } catch (const witnesstransport::Failure&) {
                    // bind may be visible before listen; retry readiness below.
                    std::this_thread::yield();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        throw std::runtime_error("service not ready");
    }
    void stop() {
        require(::kill(pid, SIGTERM) == 0, "service signal");
        int status = 0;
        require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "service stop");
        pid = -1;
    }
    const Profile&        profile;
    std::filesystem::path binary;
    pid_t                 pid = -1;
};
void emit(const Profile& profile, int boot) {
    const std::string  stream = "device/boot-" + std::to_string(boot);
    UnixAnchorProvider provider{profile.client()};
    auto               medium = FileStorageMedium::create({.directory       = profile.root / "journal",
                                                           .maxSegments     = 16,
                                                           .maxSegmentBytes = 8192,
                                                           .maxReadBytes    = 8192,
                                                           .durability      = FileDurability::QualifiedFsync});
    require(medium.has_value(), "journal open");
    StorageConfig config;
    config.segmentSize        = 8192;
    config.segmentCount       = 16;
    config.maxProducerStreams = 1;
    config.ledger             = LedgerConfig{.streamId = "ledger/boot-" + std::to_string(boot), .time = {}};
    config.provider           = &provider;
    mddlog::core::AuditRing<4> ring{stream};
    auto                       service = AuditService::create(**medium, {.storage = config});
    require(service.has_value(), "composition");
    require((*service)->addRing(ring) == AuditServiceRegistration::Registered, "ring");
    const auto description = mddlog::core::AuditDescription::create({.action = "inventory.inspect"});
    const auto context     = mddlog::core::AuditContext::create({.target = "warehouse"});
    require(description && context, "context");
    mddlog::core::AuditBinding audit{ring, *description, *context};
    require(audit.record(mddlog::core::AuditPhase::Requested, mddlog::core::RawTime::unavailable()).wasAdmitted(), "admission");
    require((*service)->poll().handedOff == 1, "hand off");
    const auto stopped = (*service)->stop();
    require(stopped.closed, "journal closed");
    const auto anchored = provider.latest(stream);
    require(std::holds_alternative<Anchor>(anchored) && std::get<Anchor>(anchored).position == 1, "real anchored record");
}
void verify(const Profile& profile, int boot, bool enroll, bool rolledBack = false) {
    UnixAnchorProvider provider{profile.client()};
    auto               medium = FileStorageMedium::create(
        {.directory = profile.root / "image", .maxSegments = 16, .maxSegmentBytes = 8192, .maxReadBytes = 8192, .access = FileAccess::ReadOnly});
    // The reader verifies a private offline image and never takes custody of the live journal.
    require(medium.has_value(), "reader journal open");
    FileRetainedPosition checkpoint{profile.root / "reader", std::string{providerName}};
    RetainedSnapshot     retained;
    if (!enroll) {
        auto restored = checkpoint.load();
        require(restored.has_value(), "reader restore");
        retained = std::move(*restored);
    }
    AnchorVerifier verifier{provider, retained.position};
    const auto     stream = "device/boot-" + std::to_string(boot);
    const auto     stored = readStoredStream(**medium, stream);
    const auto     report = verifier.verify(stream, stored.records(), {}, stored.layout());
    require(report.verdict == (rolledBack ? Verdict::RolledBack : Verdict::Anchored), "reader verdict");
    if (!rolledBack) {
        const auto saved = enroll ? checkpoint.initialize(retained.position) : checkpoint.save(retained.position, retained.generation);
        require(saved.has_value(), "reader checkpoint");
    }
}
}  // namespace
int main(int         argc,
         const char* argv[]) {  // NOLINT(bugprone-exception-escape,cppcoreguidelines-pro-bounds-array-to-pointer-decay): entry point reports exceptions.
    try {
        if (::geteuid() != 0 || argc != 2)
            return 77;
        const std::span args{argv, static_cast<std::size_t>(argc)};
        Profile         profile;
        child(witnessUid, [&] {
            require(FileAnchorAuthority::initialize(profile.authority()).has_value(), "enroll witness");
        });
        Service service{profile, args[1]};
        child(writerUid, [&] {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open uses fixed flags.
            const int descriptor = ::open((profile.root / "witness/witness.bin").c_str(), O_RDONLY | O_CLOEXEC);
            require(descriptor < 0 && errno == EACCES, "writer must not read witness files");
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open uses fixed flags.
            const int reader = ::open((profile.root / "reader").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            require(reader < 0 && errno == EACCES, "writer must not access retained position");
            UnixAnchorProvider provider{profile.client()};
            require(std::holds_alternative<ProviderUnavailable>(provider.advance(makeAnchorClaim("unowned", 1, chainInitialValue)))
                        && provider.lastError()->issue == WitnessIssue::Denied,
                    "unowned stream denied");
            emit(profile, 1);
            require(std::holds_alternative<ProviderUnavailable>(provider.retire("device/boot-1", 1)) && provider.lastError()->issue == WitnessIssue::Denied,
                    "writer retirement denied");
        });
        const auto originalJournal = profile.root / "journal";
        std::filesystem::copy(originalJournal, profile.root / "backup/journal", std::filesystem::copy_options::recursive);
        std::filesystem::copy_file(profile.root / "witness/witness.bin", profile.root / "backup/witness.bin");
        // Export offline copies to the reader; the writer's live directory keeps its authority.
        const auto exportImage = [&] {
            const auto image = profile.root / "image";
            std::filesystem::remove_all(image);
            std::filesystem::copy(originalJournal, image, std::filesystem::copy_options::recursive);
            std::filesystem::permissions(image, std::filesystem::perms::owner_all);
            require(::chown(image.c_str(), readerUid, readerUid) == 0, "image owner");
            for (const auto& entry : std::filesystem::directory_iterator(image))
                require(::chown(entry.path().c_str(), readerUid, readerUid) == 0, "image file owner");
        };
        exportImage();
        child(readerUid, [&] {
            verify(profile, 1, true);
        });
        service.stop();
        service.start();
        child(writerUid, [&] {
            emit(profile, 2);
        });
        exportImage();
        child(readerUid, [&] {
            verify(profile, 2, false);
        });
        child(readerUid, [&] {
            UnixAnchorProvider provider{profile.client()};
            require(std::holds_alternative<ProviderUnavailable>(provider.advance(makeAnchorClaim("auxiliary", 1, chainInitialValue)))
                        && provider.lastError()->issue == WitnessIssue::Denied,
                    "reader mutation denied");
        });
        child(writerUid, [&] {
            UnixAnchorProvider provider{profile.client()};
            require(std::holds_alternative<AnchorStamp>(provider.advance(makeAnchorClaim("auxiliary", 1, chainInitialValue))), "advance before retire");
        });
        child(writerUid, [&] {
            UnixAnchorProvider first{profile.client()};
            UnixAnchorProvider second{profile.client()};
            std::barrier       start{3};
            AdvanceAnswer      left{ProviderUnavailable{}};
            AdvanceAnswer      right{ProviderUnavailable{}};
            std::jthread       one([&] {
                start.arrive_and_wait();
                left = first.advance(makeAnchorClaim("concurrent", 1, chainInitialValue));
            });
            std::jthread       two([&] {
                start.arrive_and_wait();
                right = second.advance(makeAnchorClaim("concurrent", 1, chainInitialValue));
            });
            start.arrive_and_wait();
            one.join();
            two.join();
            require(std::holds_alternative<AnchorStamp>(left) != std::holds_alternative<AnchorStamp>(right), "one concurrent acceptance");
            const auto& refused = std::holds_alternative<AnchorStamp>(left) ? right : left;
            require(std::holds_alternative<AdvanceRefusal>(refused) && std::get<AdvanceRefusal>(refused) == AdvanceRefusal::PositionNotIncreasing,
                    "concurrent duplicate refused");
        });
        child(retireUid, [&] {
            UnixAnchorProvider provider{profile.client()};
            require(std::holds_alternative<AnchorStamp>(provider.retire("auxiliary", 1)), "retire authority");
        });
        service.stop();
        service.start();
        child(readerUid, [&] {
            UnixAnchorProvider provider{profile.client()};
            require(std::holds_alternative<Retirement>(provider.latest("auxiliary")), "retirement survives restart");
        });
        service.stop();
        child(writerUid, [&] {
            UnixAnchorProvider provider{profile.client()};
            require(std::holds_alternative<ProviderUnavailable>(provider.streams()), "service unavailable");
        });
        std::filesystem::remove_all(originalJournal);
        std::filesystem::copy(profile.root / "backup/journal", originalJournal, std::filesystem::copy_options::recursive);
        std::filesystem::permissions(originalJournal, std::filesystem::perms::owner_all);
        std::filesystem::copy_file(profile.root / "backup/witness.bin",
                                   profile.root / "witness/witness.bin",
                                   std::filesystem::copy_options::overwrite_existing);
        require(::chown((profile.root / "witness/witness.bin").c_str(), witnessUid, witnessUid) == 0, "restore witness owner");
        service.start();
        exportImage();
        child(readerUid, [&] {
            verify(profile, 1, false, true);
        });
        service.stop();
        std::cout << "PASS: separate UID witness, writer and reader; protected custody, permissions, independent restarts, retirement and persisted-reader "
                     "rollback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
