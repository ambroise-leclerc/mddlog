/** @brief Seeded real-file histories with an externally reconstructed admission and hash oracle. */
#include <cerrno>

#include <sys/stat.h>
import std;
import mddlog.adapter.auditservice;
import mddlog.adapter.auditlogverifier;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.fileanchorauthority;
import mddlog.adapter.fileretainedposition;

namespace {
using namespace mddlog::adapter;
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string{message});
}
std::string hex(std::span<const std::uint8_t> bytes) {
    std::string result;
    for (auto byte : bytes)
        result += std::format("{:02x}", byte);
    return result;
}
class FaultCalls final : public FileStorageCalls {
public:
    std::ptrdiff_t write(int descriptor, std::span<const std::uint8_t> bytes) override {
        if (partialPending) {
            partialPending = false;
            failWrite      = true;
            ++partialWrites;
            return FileStorageCalls::write(descriptor, bytes.first(1));
        }
        if (std::exchange(failWrite, false)) {
            errno = EIO;
            return -1;
        }
        const auto count = std::min(bytes.size(), writeBound);
        if (count < bytes.size())
            ++shortWrites;
        return FileStorageCalls::write(descriptor, bytes.first(count));
    }
    int sync(int descriptor) override {
        struct stat status{};
        require(::fstat(descriptor, &status) == 0, "fstat seam");
        if (failSync || (failDirectory && S_ISDIR(status.st_mode))) {
            failSync      = false;
            failDirectory = false;
            ++syncFailures;
            errno = EIO;
            return -1;
        }
        return FileStorageCalls::sync(descriptor);
    }
    std::size_t   writeBound     = 17;
    bool          partialPending = false;
    bool          failWrite      = false;
    bool          failSync       = false;
    bool          failDirectory  = false;
    std::uint64_t shortWrites    = 0;
    std::uint64_t partialWrites  = 0;
    std::uint64_t syncFailures   = 0;
};
class FaultProvider final : public AnchorProvider {
public:
    explicit FaultProvider(AnchorProvider& authority) : inner(authority) {}
    AdvanceAnswer advance(const AnchorClaim& claim) override {
        if (!available)
            return ProviderUnavailable{};
        auto answer = inner.advance(claim);
        if (loseNext && std::holds_alternative<AnchorStamp>(answer)) {
            loseNext = false;
            ++lostResponses;
            return ProviderUnavailable{};
        }
        return answer;
    }
    RetireAnswer retire(std::string_view stream, std::uint64_t position) override {
        return available ? inner.retire(stream, position) : RetireAnswer{ProviderUnavailable{}};
    }
    LatestAnswer latest(std::string_view stream) override {
        return available ? inner.latest(stream) : LatestAnswer{ProviderUnavailable{}};
    }
    StreamsAnswer streams() override {
        return available ? inner.streams() : StreamsAnswer{ProviderUnavailable{}};
    }
    bool          available     = false;
    bool          loseNext      = false;
    std::uint64_t lostResponses = 0;

private:
    AnchorProvider& inner;
};

/** @brief Arm the directory-barrier failure at reclaim; other pure virtual operations delegate to the real medium. */
class FaultMedium final : public StorageMedium {
public:
    FaultMedium(StorageMedium& target, FaultCalls& systemCalls) : inner(target), calls(systemCalls) {}
    OpenAnswer open(const SegmentOpening& opening) override {
        return inner.open(opening);
    }
    AppendAnswer append(SegmentRef segment, std::span<const std::uint8_t> bytes) override {
        return inner.append(segment, bytes);
    }
    SyncAnswer sync(SegmentRef segment, std::uint64_t offset) override {
        return inner.sync(segment, offset);
    }
    std::optional<std::vector<std::uint8_t>> read(SegmentRef segment, std::uint64_t offset, std::uint64_t length) override {
        return inner.read(segment, offset, length);
    }
    std::optional<std::vector<SegmentInfo>> segments() override {
        return inner.segments();
    }
    bool reclaim(SegmentRef segment) override {
        if (std::exchange(interruptReclaim, false))
            calls.failDirectory = true;
        return inner.reclaim(segment);
    }
    bool interruptReclaim = false;

private:
    StorageMedium& inner;
    FaultCalls&    calls;
};

void snapshot(std::ostream& trace, StorageMedium& medium, std::string_view stream) {
    const auto stored = readStoredStream(medium, stream);
    require(stored.complete(), "complete baseline scan");
    for (const auto& record : stored.records())
        trace << std::format("{{\"op\":\"record\",\"stream\":\"{}\",\"bytes\":\"{}\",\"digest\":\"{}\"}}\n", stream, hex(record.bytes), hex(record.digest));
    trace << std::format("{{\"op\":\"snapshot\",\"stream\":\"{}\",\"count\":{}}}\n", stream, stored.records().size());
}
void copyDirectory(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::filesystem::remove_all(destination);
    std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive);
}

void campaign(const std::filesystem::path& root, unsigned seed, unsigned boots, unsigned records, unsigned outage) {
    const auto journal = root / "journal";
    const auto witness = root / "witness";
    const auto reader  = root / "reader";
    for (const auto& path : {journal, witness, reader}) {
        std::filesystem::create_directory(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    }
    std::ofstream trace{root / "trace.jsonl"};
    trace.exceptions(std::ios::badbit | std::ios::failbit);
    WitnessStoreConfig authorityConfig{.directory = witness, .providerId = "campaign-witness"};
    require(FileAnchorAuthority::initialize(authorityConfig).has_value(), "witness enrolment");
    FileStorageConfig    fileConfig{.directory       = journal,
                                    .maxSegments     = 512,
                                    .maxSegmentBytes = 4096,
                                    .maxReadBytes    = 4096,
                                    .durability      = FileDurability::QualifiedFsync};
    FileRetainedPosition checkpoint{reader, "campaign-witness"};
    RetainedPosition     retained;
    std::uint64_t        generation = 0;
    std::mt19937         random{seed};
    std::uint64_t        shortWrites = 0;
    for (unsigned boot = 0; boot <= boots; ++boot) {
        FaultCalls calls;
        calls.writeBound = 7 + (random() % 31);
        auto authority   = FileAnchorAuthority::open(authorityConfig);
        require(authority.has_value(), "witness reopen");
        FaultProvider provider{**authority};
        auto          medium = FileStorageMedium::create(fileConfig, &calls);
        require(medium.has_value(), "journal reopen");
        FaultMedium   faultMedium{**medium, calls};
        auto          now = std::chrono::steady_clock::time_point{};
        StorageConfig storage;
        storage.segmentCount       = 512;
        storage.segmentSize        = 4096;
        storage.maxProducerStreams = 2;
        storage.sync               = {.recordBound = 3, .ageBound = std::chrono::milliseconds{5}};
        storage.clock              = [&now] {
            return now;
        };
        storage.ledger   = LedgerConfig{.streamId = "ledger/" + std::to_string(boot), .time = {}};
        storage.provider = &provider;
        auto made        = AuditService::create(
            faultMedium,
            {.storage = storage, .maxAttemptsPerPoll = 1, .maxAnchorStreamsPerPoll = 1, .anchorPeriod = std::chrono::milliseconds{1}});
        require(made.has_value(), "service restart");
        auto& service = **made;
        if (boot != 0) {
            const auto  previous = "producer/" + std::to_string(boot - 1);
            LogVerifier recoveredVerifier{**medium, **authority, retained};
            const auto  recovered = recoveredVerifier.verify();
            require(recovered.find(previous) != nullptr && recovered.find(previous)->report.verdict == Verdict::Anchored
                        && recovered.find(previous)->report.anchoredThrough == records,
                    "restart preserves confirmed/anchored boundary");
            const auto held = readStoredStream(**medium, previous);
            for (const auto& record : held.records())
                trace << std::format("{{\"op\":\"recovered\",\"stream\":\"{}\",\"bytes\":\"{}\",\"digest\":\"{}\"}}\n",
                                     previous,
                                     hex(record.bytes),
                                     hex(record.digest));
            mddlog::core::AuditEvent reused;
            require(reused.assign({.action = "campaign.record", .target = "test"}, previous, 1).wasAdmitted(), "old identity request valid");
            require(!service.storageSink().accept(reused), "restart refuses old producer identity");
        }
        if (boot == boots)
            break;
        const std::string          stream = "producer/" + std::to_string(boot);
        mddlog::core::AuditRing<4> ring{stream};
        require(service.addRing(ring) == AuditServiceRegistration::Registered, "register producer");
        std::uint64_t admitted = 0;
        std::uint64_t refusals = 0;
        const auto    admit    = [&] {
            const auto detail  = std::format("seed:{}:boot:{}:event:{}:", seed, boot, admitted + 1) + std::string(160, 'x');
            const auto pending = admitted - service.health().delivery.handedOff;
            const auto result  = ring.tryRecord({.action = "campaign.record", .target = "test", .sourceSequence = admitted + 1, .detail = detail});
            if (result.wasAdmitted()) {
                require(pending < 4, "admission agrees with abstract capacity");
                ++admitted;
                require(result.sequence() == admitted, "refusals consume no identity");
                trace << std::format("{{\"op\":\"admit\",\"stream\":\"{}\",\"sequence\":{},\"detail\":\"{}\"}}\n", stream, admitted, detail);
            } else {
                require(result.refusal() && result.refusal()->reason == mddlog::core::AuditRefusalReason::RingFull, "explicit saturation");
                require(pending == 4, "refusal agrees with abstract capacity");
                ++refusals;
                trace << std::format("{{\"op\":\"refuse\",\"stream\":\"{}\"}}\n", stream);
            }
        };
        const auto poll = [&] {
            now += std::chrono::milliseconds{1};
            (void)service.poll();
            const auto health = service.health();
            require(health.delivery.handedOff + health.delivery.pendingInRings == admitted, "all admissions remain accounted for");
            require(health.delivery.reportedLosses == 0 && health.positionOrderViolations == 0, "no silent loss or false claim");
            trace << std::format("{{\"op\":\"poll\",\"stream\":\"{}\",\"handed\":{},\"pending\":{},\"durable\":{}}}\n",
                                 stream,
                                 health.delivery.handedOff,
                                 health.delivery.pendingInRings,
                                 service.storageSink().durablePosition(stream));
        };
        for (unsigned index = 0; index < 4; ++index)
            admit();
        for (unsigned index = 0; index < outage; ++index)
            admit();  // persistent saturation before the single consumer runs
        while (admitted < records) {
            if (random() % 3 == 0)
                poll();
            else
                admit();
        }
        while (service.health().delivery.pendingInRings != 0)
            poll();
        service.storageSink().flush();
        require(service.storageSink().durablePosition(stream) == records, "flush confirms exactly admitted prefix");
        for (unsigned index = 0; index < outage; ++index)
            poll();
        require(std::holds_alternative<AnchorAbsent>((*authority)->latest(stream)), "outage never manufactures an anchor");
        require(ring.refusalCount() == refusals && refusals >= outage, "exact prolonged refusal counter");
        provider.available = true;
        provider.loseNext  = true;
        require(!service.storageSink().advanceAnchor(stream) && provider.lostResponses == 1, "accepted real anchor response lost");
        require(service.storageSink().advanceAnchor(stream), "latest reconciles the lost response");
        const auto anchor = (*authority)->latest(stream);
        require(std::holds_alternative<Anchor>(anchor) && std::get<Anchor>(anchor).position == records, "real witness position");
        snapshot(trace, **medium, stream);
        trace << std::format("{{\"op\":\"anchor\",\"stream\":\"{}\",\"position\":{},\"digest\":\"{}\",\"refusals\":{}}}\n",
                             stream,
                             records,
                             hex(std::get<Anchor>(anchor).digest),
                             refusals);
        LogVerifier verifier{**medium, **authority, retained};
        const auto  report = verifier.verify();
        require(report.find(stream) != nullptr && report.find(stream)->report.verdict == Verdict::Anchored, "anchored baseline");
        const auto saved = generation == 0 ? checkpoint.initialize(retained) : checkpoint.save(retained, generation);
        require(saved.has_value(), "persist reader checkpoint");
        generation          = *saved;
        const unsigned mode = (seed + boot) % 3;
        if (mode == 2) {
            faultMedium.interruptReclaim = true;
            const auto trim              = service.storageSink().trimPrefix(stream);
            require(trim.outcome == RetentionOutcome::ReclaimInterrupted && calls.syncFailures == 1, "unlink followed by failed directory barrier");
            require(trim.trimmedThrough > 0 && trim.trimmedThrough <= records, "retention bounded by independent admitted/anchored prefix");
            trace << std::format("{{\"op\":\"fault\",\"kind\":\"reclaim\",\"stream\":\"{}\",\"through\":{}}}\n", stream, trim.trimmedThrough);
        } else {
            const auto trim = service.storageSink().trimPrefix(stream);
            require(trim.outcome == RetentionOutcome::Trimmed && trim.trimmedThrough > 0 && trim.trimmedThrough <= records, "whole-prefix rotation");
            provider.available = false;
            if (mode == 0)
                calls.partialPending = true;
            else
                calls.failSync = true;
            admit();
            (void)service.poll();
            if (mode == 1) {
                service.storageSink().flush();
            }
            require(mode == 0 ? calls.partialWrites == 1 : calls.syncFailures == 1, "terminal seam exercised");
            require(service.storageSink().durablePosition(stream) == records, "fault cannot raise durable prefix");
            trace << std::format("{{\"op\":\"fault\",\"kind\":\"{}\",\"stream\":\"{}\",\"through\":{}}}\n",
                                 mode == 0 ? "append" : "sync",
                                 stream,
                                 trim.trimmedThrough);
        }
        const auto terminal = service.health();
        const auto pending  = terminal.delivery.pendingInRings;
        require(terminal.delivery.takenUnacknowledged <= pending && terminal.delivery.handedOff + pending == admitted,
                "terminal fault preserves admission accounting");
        require(terminal.delivery.reportedLosses == terminal.delivery.handedOff - records, "every unconfirmed handoff is explicitly reported lost");
        trace << std::format("{{\"op\":\"fault_health\",\"stream\":\"{}\",\"handed\":{},\"pending\":{},\"losses\":{},\"durable\":{}}}\n",
                             stream,
                             terminal.delivery.handedOff,
                             pending,
                             terminal.delivery.reportedLosses,
                             service.storageSink().durablePosition(stream));
        shortWrites += calls.shortWrites;
        // Destruction deliberately omits orderly stop: the next boot must recover the interrupted session.
        made->reset();
        medium->reset();
        authority->reset();
        if (boot == 0) {
            copyDirectory(journal, root / "baseline-journal");
            copyDirectory(witness, root / "baseline-witness");
        }
        trace.flush();
    }
    // A real reader restart retains newer witness counters across a combined rollback of both writers.
    auto restored = checkpoint.load();
    require(restored.has_value() && restored->generation == generation, "reader restart preserves generation");
    copyDirectory(root / "baseline-journal", journal);
    copyDirectory(root / "baseline-witness", witness);
    auto authority = FileAnchorAuthority::open(authorityConfig);
    auto medium    = FileStorageMedium::create(fileConfig);
    require(authority.has_value() && medium.has_value(), "rollback image opens");
    LogVerifier verifier{**medium, **authority, restored->position};
    const auto  rollback = verifier.verify();
    require(std::ranges::any_of(rollback.streams,
                                [](const auto& item) {
                                    return item.report.verdict == Verdict::RolledBack;
                                }),
            "persistent reader detects combined rollback");
    trace << std::format("{{\"op\":\"complete\",\"boots\":{},\"short_writes\":{},\"rollback\":true}}\n", boots, shortWrites);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::span args{argv, static_cast<std::size_t>(argc)};
        if (args.size() != 6)
            return 2;
        const auto seed    = static_cast<unsigned>(std::stoul(args[2]));
        const auto boots   = static_cast<unsigned>(std::stoul(args[3]));
        const auto records = static_cast<unsigned>(std::stoul(args[4]));
        const auto outage  = static_cast<unsigned>(std::stoul(args[5]));
        require(seed > 0 && boots >= 3 && boots <= 24 && records >= 32 && records <= 64 && outage >= 1 && outage <= 1000, "campaign budget");
        campaign(args[1], seed, boots, records, outage);
        std::println("PASS seed={} boots={} records={} outage={}", seed, boots, records, outage);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
