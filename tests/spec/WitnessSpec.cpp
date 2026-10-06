/** @brief Durable authority and real Unix transport conformance, deadlines and lost acknowledgements (#115). */
#include <cerrno>
#include <cstdlib>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
import std;
import mddlog.adapter.auditchain;
import speclab;
import mddlog.adapter.fileanchorauthority;
import mddlog.adapter.unixanchorprovider;
import mddlog.adapter.witnesscodec;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.sha256;

namespace {
using namespace mddlog::adapter;
using namespace std::chrono_literals;
struct Directory {
    Directory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "mddlog-witness-XXXXXX").string();
        const char* made    = ::mkdtemp(pattern.data());
        if (made == nullptr)
            throw std::runtime_error("directory");
        path = made;
    }
    Directory(const Directory&)            = delete;
    Directory& operator=(const Directory&) = delete;
    Directory(Directory&&)                 = delete;
    Directory& operator=(Directory&&)      = delete;
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    [[nodiscard]] WitnessStoreConfig config() const {
        return {.directory = path, .providerId = "witness"};
    }
    std::filesystem::path path;
};
void require(bool condition, std::string_view message = "witness test precondition") {
    if (!condition)
        throw std::runtime_error(std::string{message});
}
[[nodiscard]] Anchor fixtureAnchor(std::string streamId, std::string providerId, std::uint64_t counter) {
    return {.anchorFormat     = anchorFormatVersion,
            .canonicalVersion = canonicalContractVersion,
            .streamId         = std::move(streamId),
            .position         = 1,
            .digest           = chainInitialValue,
            .providerId       = std::move(providerId),
            .counter          = counter};
}
[[nodiscard]] std::vector<std::uint8_t> stateBytes(const ProviderListing& listing) {
    witness::Encoder encoder;
    encoder.text("mddlog-witness");
    encoder.number(witness::protocolVersion);
    encoder.listing(listing);
    const auto checksum = sha256(encoder.bytes);
    encoder.raw(checksum);
    return encoder.bytes;
}
void seedState(const Directory& directory, const ProviderListing& listing) {
    const auto    bytes = stateBytes(listing);
    std::ofstream output(directory.path / "witness.bin", std::ios::binary | std::ios::trunc);
    for (const auto byte : bytes)
        output.put(static_cast<char>(byte));
    output.close();
    require(!output.fail(), "seed durable fixture state");
}
struct RunningService {
    explicit RunningService(AnchorProvider& authority, const std::filesystem::path& path)
        : socketPath(path / "socket"), service(make(authority)), worker([this](const std::stop_token& stop) {
              while (!stop.stop_requested())
                  (void)service->serveOnce();
          }) {}
    RunningService(const RunningService&)            = delete;
    RunningService& operator=(const RunningService&) = delete;
    RunningService(RunningService&&)                 = delete;
    RunningService& operator=(RunningService&&)      = delete;
    ~RunningService() {
        worker.request_stop();
        worker.join();
    }
    [[nodiscard]] UnixWitnessConfig config() const {
        return {.socketPath = socketPath, .providerId = "witness", .serverUid = ::geteuid(), .timeout = 1s};
    }
    [[nodiscard]] std::unique_ptr<UnixWitnessService> make(AnchorProvider& authority) const {
        auto made = UnixWitnessService::create(
            {
                .socketPath  = socketPath,
                .providerId  = "witness",
                .readers     = {::geteuid()},
                .permissions = {{.uid = ::geteuid(), .streamId = "allowed", .advance = true},
                                {.uid = ::geteuid(), .streamId = "retirable", .advance = true, .retire = true}},
                .timeout     = 100ms
        },
            authority);
        require(made.has_value());
        return std::move(*made);
    }
    std::filesystem::path               socketPath;
    std::unique_ptr<UnixWitnessService> service;
    std::jthread                        worker;
};
struct FaultCalls final : FileStorageCalls {
    int sync(int descriptor) override {
        ++count;
        if (count == failSync) {
            errno = EIO;
            return -1;
        }
        return FileStorageCalls::sync(descriptor);
    }
    std::ptrdiff_t write(int descriptor, std::span<const std::uint8_t> bytes) override {
        ++writeCount;
        if (failWrite) {
            errno = ENOSPC;
            return -1;
        }
        return FileStorageCalls::write(descriptor, bytes.first(std::min(bytes.size(), std::size_t{11})));
    }
    int  count      = 0;
    int  writeCount = 0;
    int  failSync   = 0;
    bool failWrite  = false;
};
// NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init): registry stores a lambda; stream modes are only used during scenario execution.
const speclab::Register durable{
    "Witness authority preserves monotonically accepted state and irreversible retirements",
    "unit",
    [] {
        return speclab::Test("witness-durable-authority")
            .Then("enrolment is explicit; reopening preserves counters and conflicts",
                  [] {
                      Directory  directory;
                      const auto missing = FileAnchorAuthority::open(directory.config());
                      require(!missing && missing.error().issue == WitnessStoreIssue::Missing);
                      require(FileAnchorAuthority::initialize(directory.config()).has_value());
                      require(!FileAnchorAuthority::initialize(directory.config()));
                      {
                          auto made = FileAnchorAuthority::open(directory.config());
                          require(made.has_value());
                          auto&      authority = **made;
                          const auto locked    = FileAnchorAuthority::open(directory.config());
                          require(!locked && locked.error().issue == WitnessStoreIssue::Busy);
                          const auto accepted = authority.advance(makeAnchorClaim("retirable", 1, chainInitialValue));
                          require(std::get<AnchorStamp>(accepted).counter == 1);
                          const auto retired = authority.retire("retirable", 1);
                          require(std::get<AnchorStamp>(retired).counter == 2);
                      }
                      auto made = FileAnchorAuthority::open(directory.config());
                      require(made.has_value());
                      auto&                 authority = **made;
                      speclab::core::Checks checks;
                      const auto            listing = std::get<ProviderListing>(authority.streams());
                      checks.expect(listing.head == 2 && std::holds_alternative<Retirement>(listing.entries.at(0)),
                                    "retirement and head survive service restart");
                      checks.expect(std::get<RetireRefusal>(authority.retire("retirable", 1)) == RetireRefusal::Conflict, "no duplicate retirement accepted");
                      checks.expect(std::get<AdvanceRefusal>(authority.advance(makeAnchorClaim("retirable", 2, chainInitialValue))) == AdvanceRefusal::Conflict,
                                    "no revival");
                      checks.expect(std::get<AnchorStamp>(authority.advance(makeAnchorClaim("allowed", 1, chainInitialValue))).counter == 3,
                                    "counter rises after restart");
                      checks.raise();
                  })
            .Then("stream capacity refuses new identities without stopping reads or existing-stream mutations",
                  [] {
                      Directory directory;
                      require(FileAnchorAuthority::initialize(directory.config()).has_value());
                      ProviderListing listing{.providerId = "witness", .head = witness::maxStreams, .entries = {}};
                      for (std::size_t index = 0; index < witness::maxStreams; ++index)
                          listing.entries.emplace_back(fixtureAnchor(std::format("s{:04}", index), listing.providerId, index + 1));
                      seedState(directory, listing);
                      FaultCalls calls;
                      auto       made = FileAnchorAuthority::open(directory.config(), &calls);
                      require(made.has_value());
                      calls.count = 0;
                      require(std::holds_alternative<ProviderUnavailable>((*made)->advance(makeAnchorClaim("new", 1, chainInitialValue))));
                      require(!(*made)->lastError() && calls.writeCount == 0 && calls.count == 0);
                      require(std::get<ProviderListing>((*made)->streams()).head == witness::maxStreams);
                      require(std::holds_alternative<Anchor>((*made)->latest("s0000")));
                      require(std::holds_alternative<AnchorStamp>((*made)->advance(makeAnchorClaim("s0000", 2, chainInitialValue))));
                      require(std::holds_alternative<AnchorStamp>((*made)->retire("s0000", 2)));
                      require(std::holds_alternative<ProviderUnavailable>((*made)->advance(makeAnchorClaim("new", 1, chainInitialValue))));
                      require(!(*made)->lastError() && std::holds_alternative<Retirement>((*made)->latest("s0000")));
                  })
            .Then("counter exhaustion refuses advance and retirement without losing the readable state",
                  [] {
                      Directory directory;
                      require(FileAnchorAuthority::initialize(directory.config()).has_value());
                      const auto maximum = std::numeric_limits<std::uint64_t>::max();
                      seedState(directory, {.providerId = "witness", .head = maximum, .entries = {fixtureAnchor("allowed", "witness", maximum)}});
                      FaultCalls calls;
                      auto       made = FileAnchorAuthority::open(directory.config(), &calls);
                      require(made.has_value());
                      calls.count = 0;
                      require(std::holds_alternative<ProviderUnavailable>((*made)->advance(makeAnchorClaim("allowed", 2, chainInitialValue))));
                      require(std::holds_alternative<ProviderUnavailable>((*made)->retire("allowed", 1)));
                      require(!(*made)->lastError() && calls.writeCount == 0 && calls.count == 0);
                      require(std::get<ProviderListing>((*made)->streams()).head == maximum);
                      require(std::get<Anchor>((*made)->latest("allowed")).position == 1);
                  })
            .Then("encoded-size exhaustion performs no write and leaves the authority readable",
                  [] {
                      Directory directory;
                      auto      config  = directory.config();
                      config.providerId = std::string(witness::maxTextBytes, 'p');
                      require(FileAnchorAuthority::initialize(config).has_value());
                      ProviderListing listing{.providerId = config.providerId, .head = 0, .entries = {}};
                      const auto      emptySize = stateBytes(listing).size();
                      listing.head              = 1;
                      listing.entries.emplace_back(fixtureAnchor("s0000", config.providerId, 1));
                      const auto entrySize = stateBytes(listing).size() - emptySize;
                      const auto count     = (witness::maxBytes - emptySize) / entrySize;
                      require(count < witness::maxStreams);
                      for (std::size_t index = 1; index < count; ++index)
                          listing.entries.emplace_back(fixtureAnchor(std::format("s{:04}", index), config.providerId, index + 1));
                      listing.head = count;
                      seedState(directory, listing);
                      FaultCalls calls;
                      auto       made = FileAnchorAuthority::open(config, &calls);
                      require(made.has_value());
                      calls.count = 0;
                      require(std::holds_alternative<ProviderUnavailable>((*made)->advance(makeAnchorClaim("s9999", 1, chainInitialValue))));
                      require(!(*made)->lastError() && calls.writeCount == 0 && calls.count == 0);
                      require(std::get<ProviderListing>((*made)->streams()).head == count);
                      require(std::holds_alternative<Anchor>((*made)->latest("s0000")));
                  })
            .Then("failed durable mutations stop the authority and visible renames are reconciled on reopening",
                  [] {
                      for (int stage = 0; stage < 3; ++stage) {
                          Directory directory;
                          require(FileAnchorAuthority::initialize(directory.config()).has_value());
                          {
                              FaultCalls calls;
                              auto       made = FileAnchorAuthority::open(directory.config(), &calls);
                              require(made.has_value());
                              calls.count     = 0;
                              calls.failWrite = stage == 0;
                              calls.failSync  = stage;
                              require(std::holds_alternative<ProviderUnavailable>((*made)->advance(makeAnchorClaim("allowed", 1, chainInitialValue))));
                              require(std::holds_alternative<ProviderUnavailable>((*made)->streams()));
                              require((*made)->lastError().has_value());
                          }
                          auto reopened = FileAnchorAuthority::open(directory.config());
                          require(reopened.has_value());
                          const auto            listing = std::get<ProviderListing>((*reopened)->streams());
                          speclab::core::Checks checks;
                          checks.expect(listing.head == (stage == 2 ? std::uint64_t{1} : std::uint64_t{0}), "only renamed state survives");
                          checks.raise();
                      }
                  })
            .Then("identity mismatch, corruption and loss cannot become an empty provider",
                  [] {
                      Directory directory;
                      require(FileAnchorAuthority::initialize(directory.config()).has_value());
                      auto config         = directory.config();
                      config.providerId   = "other";
                      const auto mismatch = FileAnchorAuthority::open(config);
                      require(!mismatch && mismatch.error().issue == WitnessStoreIssue::IdentityMismatch);
                      {
                          std::ofstream output(directory.path / "witness.bin", std::ios::binary | std::ios::trunc);
                          output << "broken";
                      }
                      const auto damaged = FileAnchorAuthority::open(directory.config());
                      require(!damaged && damaged.error().issue == WitnessStoreIssue::Invalid);
                      require(!FileAnchorAuthority::initialize(directory.config()));
                      std::filesystem::remove(directory.path / "witness.bin");
                      const auto missing = FileAnchorAuthority::open(directory.config());
                      require(!missing && missing.error().issue == WitnessStoreIssue::Missing);
                  })
            .Execute();
    }};
const speclab::Register transport{
    "Unix witness authenticates peers, enforces authority and reconciles lost replies",
    "unit",
    [] {
        return speclab::Test("witness-unix-transport")
            .Then("advance, latest, streams and retire use the enrolled service; unauthorized streams and roles fail",
                  [] {
                      Directory directory;
                      require(FileAnchorAuthority::initialize(directory.config()).has_value());
                      auto made = FileAnchorAuthority::open(directory.config());
                      require(made.has_value());
                      RunningService        service{**made, directory.path};
                      UnixAnchorProvider    client{service.config()};
                      speclab::core::Checks checks;
                      checks.expect(std::holds_alternative<AnchorAbsent>(client.latest("allowed")), "authenticated absence");
                      checks.expect(std::get<AdvanceRefusal>(client.advance(makeAnchorClaim("allowed", 0, chainInitialValue))) == AdvanceRefusal::Malformed,
                                    "malformed claim refused before transport");
                      const auto accepted = client.advance(makeAnchorClaim("allowed", 1, chainInitialValue));
                      checks.expect(std::holds_alternative<AnchorStamp>(accepted), "durable acceptance");
                      const auto latest = client.latest("allowed");
                      checks.expect(std::holds_alternative<Anchor>(latest), "latest real anchor");
                      checks.expect(std::holds_alternative<ProviderUnavailable>(client.advance(makeAnchorClaim("unowned", 1, chainInitialValue)))
                                        && client.lastError()->issue == WitnessIssue::Denied,
                                    "stream advance permission");
                      checks.expect(std::holds_alternative<ProviderUnavailable>(client.retire("allowed", 1))
                                        && client.lastError()->issue == WitnessIssue::Denied,
                                    "separate retirement authority");
                      require(std::holds_alternative<AnchorStamp>(client.advance(makeAnchorClaim("retirable", 1, chainInitialValue))));
                      require(std::holds_alternative<AnchorStamp>(client.retire("retirable", 1)));
                      checks.expect(std::holds_alternative<Retirement>(client.latest("retirable")), "permanent retirement");
                      checks.expect(std::get<ProviderListing>(client.streams()).head == 3, "authorized listing");
                      auto wrong = service.config();
                      ++wrong.serverUid;
                      UnixAnchorProvider imposter{wrong};
                      checks.expect(std::holds_alternative<ProviderUnavailable>(imposter.streams())
                                        && imposter.lastError()->issue == WitnessIssue::Authentication,
                                    "wrong server UID rejected before request");
                      checks.raise();
                  })
            .Then("unknown operation tags and trailing fields never mutate the authority",
                  [] {
                      Directory directory;
                      require(FileAnchorAuthority::initialize(directory.config()).has_value());
                      auto made = FileAnchorAuthority::open(directory.config());
                      require(made.has_value());
                      RunningService service{**made, directory.path};
                      for (const bool extra : {false, true}) {
                          const auto       deadline = std::chrono::steady_clock::now() + 1s;
                          auto             socket   = witnesstransport::connect(service.config(), deadline);
                          witness::Encoder request;
                          request.number(witness::protocolVersion);
                          request.number(extra ? static_cast<std::uint64_t>(witness::Operation::Advance) : 257);
                          request.text("witness");
                          request.number(anchorFormatVersion);
                          request.number(canonicalContractVersion);
                          request.text("allowed");
                          request.number(1);
                          request.raw(chainInitialValue);
                          if (extra)
                              request.number(0);
                          witnesstransport::sendFrame(socket.get(), request.bytes, deadline);
                          bool rejected = false;
                          try {
                              (void)witnesstransport::receiveFrame(socket.get(), deadline);
                          } catch (const witnesstransport::Failure&) {
                              rejected = true;
                          }
                          require(rejected, "malformed request rejected");
                      }
                      UnixAnchorProvider client{service.config()};
                      require(std::get<ProviderListing>(client.streams()).head == 0, "malformed requests leave head unchanged");
                  })
            .Then("a dropped acknowledgement is reconciled without making conflicts idempotent",
                  [] {
                      Directory directory;
                      require(FileAnchorAuthority::initialize(directory.config()).has_value());
                      auto made = FileAnchorAuthority::open(directory.config());
                      require(made.has_value());
                      RunningService service{**made, directory.path};
                      {
                          const auto       deadline = std::chrono::steady_clock::now() + 1s;
                          auto             socket   = witnesstransport::connect(service.config(), deadline);
                          witness::Encoder request;
                          request.number(witness::protocolVersion);
                          request.number(static_cast<std::uint64_t>(witness::Operation::Advance));
                          request.text("witness");
                          request.number(anchorFormatVersion);
                          request.number(canonicalContractVersion);
                          request.text("allowed");
                          request.number(1);
                          request.raw(chainInitialValue);
                          witnesstransport::sendFrame(socket.get(), request.bytes, deadline);
                          // Closing the client discards the response, while the complete request remains queued.
                      }
                      UnixAnchorProvider client{service.config()};
                      const auto         latest = client.latest("allowed");
                      require(std::holds_alternative<Anchor>(latest));
                      speclab::core::Checks checks;
                      checks.expect(std::get<Anchor>(latest).counter == 1, "authentic reconciliation finds committed claim");
                      checks.expect(std::get<AdvanceRefusal>(client.advance(makeAnchorClaim("allowed", 1, chainInitialValue)))
                                        == AdvanceRefusal::PositionNotIncreasing,
                                    "duplicate still refused");
                      auto digest    = chainInitialValue;
                      digest.front() = 1;
                      checks.expect(std::get<AdvanceRefusal>(client.advance(makeAnchorClaim("allowed", 1, digest))) == AdvanceRefusal::Conflict,
                                    "diverging retry refused");
                      require(std::holds_alternative<AnchorStamp>(client.advance(makeAnchorClaim("retirable", 1, chainInitialValue))));
                      {
                          const auto       deadline = std::chrono::steady_clock::now() + 1s;
                          auto             socket   = witnesstransport::connect(service.config(), deadline);
                          witness::Encoder request;
                          request.number(witness::protocolVersion);
                          request.number(static_cast<std::uint64_t>(witness::Operation::Retire));
                          request.text("witness");
                          request.text("retirable");
                          request.number(1);
                          witnesstransport::sendFrame(socket.get(), request.bytes, deadline);
                      }
                      const auto retired = client.latest("retirable");
                      checks.expect(std::holds_alternative<Retirement>(retired) && std::get<Retirement>(retired).counter == 3,
                                    "lost retirement response reconciled");
                      checks.expect(std::get<RetireRefusal>(client.retire("retirable", 1)) == RetireRefusal::Conflict,
                                    "duplicate retirement remains a refusal");
                      checks.raise();
                  })
            .Then("a full Unix backlog is a connection failure rather than an authentication failure",
                  [] {
                      Directory              directory;
                      const auto             path = directory.path / "full";
                      detail::FileDescriptor listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
                      const auto             address = witnesstransport::addressOf(path);
                      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket address ABI.
                      require(::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && ::listen(listener.get(), 0) == 0);
                      detail::FileDescriptor queued(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
                      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket address ABI.
                      require(::connect(queued.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
                      UnixAnchorProvider client{
                          {.socketPath = path, .providerId = "witness", .serverUid = ::geteuid(), .timeout = 20ms}
                      };
                      require(std::holds_alternative<ProviderUnavailable>(client.streams()));
                      require(client.lastError()->issue == WitnessIssue::Connect && client.lastError()->nativeError == EAGAIN);
                  })
            .Then("a silent service times out and a missing service never acknowledges",
                  [] {
                      Directory              directory;
                      const auto             path = directory.path / "silent";
                      detail::FileDescriptor listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
                      const auto             address = witnesstransport::addressOf(path);
                      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket address ABI.
                      require(::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && ::listen(listener.get(), 1) == 0);
                      std::jthread       silent([&] {
                          detail::FileDescriptor socket(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
                          std::this_thread::sleep_for(100ms);
                      });
                      UnixAnchorProvider client{
                          {.socketPath = path, .providerId = "witness", .serverUid = ::geteuid(), .timeout = 20ms}
                      };
                      speclab::core::Checks checks;
                      checks.expect(std::holds_alternative<ProviderUnavailable>(client.streams()) && client.lastError()->issue == WitnessIssue::Timeout,
                                    "bounded response wait");
                      UnixAnchorProvider absent{
                          {.socketPath = directory.path / "absent", .providerId = "witness", .serverUid = ::geteuid()}
                      };
                      checks.expect(std::holds_alternative<ProviderUnavailable>(absent.streams()) && absent.lastError()->issue == WitnessIssue::Connect,
                                    "service outage");
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
