/** @brief Reproducible resource corpus shared with the v0.2 baseline. */
import std;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;
import mddlog.core.auditring;

namespace {
using namespace mddlog::adapter;
using Clock = std::chrono::steady_clock;

class MeasuredMedium final : public StorageMedium {
public:
    InMemoryStorageMedium inner{512};
    std::uint64_t         bytesRead  = 0;
    std::uint64_t         maxRequest = 0;
    std::size_t           readCalls  = 0;
    std::size_t           syncCalls  = 0;
    OpenAnswer            open(const SegmentOpening& opening) override {
        return inner.open(opening);
    }
    AppendAnswer append(SegmentRef segment, std::span<const std::uint8_t> bytes) override {
        return inner.append(segment, bytes);
    }
    SyncAnswer sync(SegmentRef segment, std::uint64_t offset) override {
        ++syncCalls;
        return inner.sync(segment, offset);
    }
    std::optional<std::vector<std::uint8_t>> read(SegmentRef segment, std::uint64_t offset, std::uint64_t length) override {
        ++readCalls;
        bytesRead += length;
        maxRequest = std::max(maxRequest, length);
        return inner.read(segment, offset, length);
    }
    std::optional<std::vector<SegmentInfo>> segments() override {
        return inner.segments();
    }
    bool reclaim(SegmentRef segment) override {
        return inner.reclaim(segment);
    }
};

// The identical source also builds on v0.2, which predates resource profiles.
template <class Config>
void applyProfile(Config& config) {
    if constexpr (requires { config.resources; }) {
        config.resources.maxSegments        = 512;
        config.resources.maxSegmentBytes    = 32768;
        config.resources.maxTotalBytes      = 16 * 1024 * 1024;
        config.resources.readChunkBytes     = 4096;
        config.resources.maxReadBytes       = 32 * 1024 * 1024;
        config.resources.maxStreams         = 256;
        config.resources.maxRecords         = 65536;
        config.resources.maxProviderEntries = 2048;
        config.resources.maxProviderCalls   = 8192;
        config.resources.maxIntegrityFaults = 2048;
    }
}

template <class Report>
bool resourceFailure(const Report& report) {
    if constexpr (requires { report.resourceIssue; })
        return static_cast<unsigned>(report.resourceIssue) != 0;
    return false;
}

template <class Config>
LogAnalysis readAnalysis(StorageMedium& medium, const Config& config) {
    if constexpr (requires { config.resources; })
        return LogAnalysis::read(medium, config.resources);
    else
        return LogAnalysis::read(medium);
}
template <class Config>
ChainStateRecovery recover(const LogAnalysis& log, AnchorProvider& provider, const Config& config) {
    if constexpr (requires { config.resources; })
        return recoverChainState(log, &provider, config.resources);
    else
        return recoverChainState(log, &provider);
}

mddlog::core::AuditEvent eventAt(std::string_view stream, std::uint64_t sequence) {
    mddlog::core::AuditEvent event;
    const std::string        detail(200, 'x');
    if (!event.assign({.action = "resource.measure", .target = "reference", .detail = detail}, stream, sequence).wasAdmitted())
        throw std::runtime_error("invalid corpus event");
    return event;
}

void emit(std::string_view operation, Clock::time_point start, std::uint64_t units) {
    std::cout << operation << ',' << std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() << ',' << units << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::size_t count = argc > 1 ? std::stoull(std::span{argv, static_cast<std::size_t>(argc)}.subspan(1).front()) : 4096;
        if (count == 0 || count > 49152)
            return 2;
        constexpr std::size_t  streamCount = 32;
        MeasuredMedium         medium;
        InMemoryAnchorProvider provider{"resource-witness"};
        StorageConfig          config;
        config.segmentSize        = 32768;
        config.segmentCount       = 512;
        config.maxProducerStreams = streamCount;
        config.sync.recordBound   = 128;
        config.ledger             = LedgerConfig{.streamId = "ledger/resources", .time = {}};
        config.provider           = &provider;
        applyProfile(config);
        const auto creation = Clock::now();
        auto       made     = PersistingAuditSink::create(medium, config);
        if (!made)
            return 3;
        auto& sink = **made;
        emit("startup_empty", creation, 0);
        const auto appendStart = Clock::now();
        for (std::size_t i = 0; i < count; ++i) {
            const auto identity = "producer/" + std::to_string(i % streamCount);
            if (!sink.accept(eventAt(identity, (i / streamCount) + 1)))
                return 4;
        }
        emit("append", appendStart, count);
        const auto closeStart = Clock::now();
        sink.close();
        emit("close", closeStart, count);
        const auto               restartStart = Clock::now();
        std::chrono::nanoseconds maximumRestart{};
        for (std::size_t session = 1; session <= 64; ++session) {
            config.ledger           = LedgerConfig{.streamId = "ledger/restart/" + std::to_string(session), .time = {}};
            const auto sessionStart = Clock::now();
            auto       next         = PersistingAuditSink::create(medium, config);
            if (!next)
                return 12;
            (*next)->close();
            maximumRestart = std::max(maximumRestart, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - sessionStart));
        }
        std::cout << "restart_max," << maximumRestart.count() << ",1\n";
        emit("restart_64", restartStart, 64);
        const auto listing = medium.segments();
        if (!listing)
            return 5;
        std::uint64_t bytes = 0;
        for (const auto& item : *listing)
            bytes += item.size;
        std::cout << "corpus_bytes,0," << bytes << '\n';
        std::cout << "corpus_segments,0," << listing->size() << '\n';
        for (std::size_t i = 0; i < 1024; ++i) {
            const auto identity = "retired/" + std::to_string(i);
            if (!std::holds_alternative<AnchorStamp>(provider.advance(makeAnchorClaim(identity, 1, chainInitialValue)))
                || !std::holds_alternative<AnchorStamp>(provider.retire(identity, 1)))
                return 6;
        }
        const auto readStart = Clock::now();
        auto       image     = readAnalysis(medium, config);
        emit("analysis", readStart, count);
        const auto recoveryStart = Clock::now();
        const auto recovered     = recover(image, provider, config);
        emit("recovery", recoveryStart, recovered.held.size());
        RetainedPosition retained;
        const auto       verifyStart = Clock::now();
        VerifierConfig   verifierConfig;
        applyProfile(verifierConfig);
        const auto report = LogVerifier{medium, provider, retained, verifierConfig}.verify();
        emit("verify", verifyStart, report.streams.size() + report.unlisted.size());
        if (resourceFailure(report) || report.mediumUnreadable || report.streams.size() < streamCount + 1)
            return 7;
        config.ledger          = LedgerConfig{.streamId = "ledger/maintenance", .time = {}};
        const auto richStartup = Clock::now();
        auto       maintenance = PersistingAuditSink::create(medium, config);
        emit("startup_rich", richStartup, 1024);
        if (!maintenance)
            return 13;
        const auto rotationStart = Clock::now();
        const auto rotation      = (*maintenance)->trimPrefix("producer/0");
        emit("rotation", rotationStart, rotation.segmentsReclaimed);
        const auto retentionStart = Clock::now();
        const auto removal        = (*maintenance)->removeStream("producer/1");
        emit("retention", retentionStart, removal.segmentsReclaimed);
        (*maintenance)->close();
        std::cout << "read_bytes,0," << medium.bytesRead << '\n';
        std::cout << "read_calls,0," << medium.readCalls << '\n';
        std::cout << "max_read_request,0," << medium.maxRequest << '\n';
        std::cout << "sync_calls,0," << medium.syncCalls << '\n';
        mddlog::core::AuditRing<1> ring{"governed"};
        const auto                 admissionStart = Clock::now();
        for (std::size_t i = 0; i < 100000; ++i) {
            if (!ring.tryRecord({.action = "resource.admit", .target = "reference"}).wasAdmitted())
                return 8;
            const auto batch = ring.drain();
            if (!ring.acknowledge(batch, 1))
                return 9;
        }
        emit("admission", admissionStart, 100000);
        if (!ring.tryRecord({.action = "resource.admit", .target = "reference"}).wasAdmitted())
            return 14;
        const auto saturation = Clock::now();
        for (std::size_t i = 0; i < 100000; ++i) {
            const auto refused = ring.tryRecord({.action = "resource.admit", .target = "reference"});
            if (refused.wasAdmitted() || !refused.refusal() || refused.refusal()->reason != mddlog::core::AuditRefusalReason::RingFull)
                return 15;
        }
        emit("saturated_admission", saturation, 100000);
        const auto                      hashStart = Clock::now();
        const std::vector<std::uint8_t> block(65536, 42);
        Sha256Digest                    digest{};
        for (std::size_t i = 0; i < 128; ++i)
            digest = sha256(block);
        if (digest == chainInitialValue)
            return 10;
        emit("sha256", hashStart, 128 * block.size());
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 11;
    }
}
