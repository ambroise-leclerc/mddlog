/** @brief File audit composition, explicit admission and offline verification after reopening (#114). */
import std;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditservice;
import mddlog.core.auditbinding;

namespace {
using namespace mddlog::adapter;
constexpr std::size_t segmentBytes = 8192;
constexpr std::size_t segmentCount = 16;

template <std::size_t Capacity>
[[nodiscard]] bool emit(mddlog::core::AuditBinding<Capacity>& audit) {
    return audit.record(mddlog::core::AuditPhase::Requested, mddlog::core::RawTime::unavailable()).wasAdmitted();
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): example reports configuration failures; unexpected allocation errors terminate the demonstration.
int main(int argc, char** argv) {
    if (argc != 3) {
        std::println(std::cerr, "Usage: FileAudit <existing-private-directory> <unique-stream-id>");
        return 1;
    }
    // Composition and lifecycle live here. This example deliberately keeps durability unqualified.
    FileStorageConfig files{.directory   = std::filesystem::path(argv[1]),  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is bounded by argc.
                            .maxSegments = segmentCount,
                            .maxSegmentBytes = segmentBytes,
                            .maxReadBytes    = segmentBytes};
    const std::string stream(argv[2]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is bounded by argc.
    {
        auto made = FileStorageMedium::create(files);
        if (!made) {
            std::println(std::cerr, "Cannot open storage: issue {}, errno {}", std::to_underlying(made.error().issue), made.error().nativeError);
            return 1;
        }
        auto&         medium = **made;
        StorageConfig storage;
        storage.segmentSize        = segmentBytes;
        storage.segmentCount       = segmentCount;
        storage.maxProducerStreams = 1;
        storage.ledger             = LedgerConfig{.streamId = stream + ":ledger", .time = {}};
        mddlog::core::AuditRing<4> ring(stream);
        auto                       service = AuditService::create(medium, {.storage = storage});
        if (!service) {
            std::println(std::cerr, "Cannot configure audit service: issue {}", std::to_underlying(service.error().issue));
            return 1;
        }
        if ((*service)->addRing(ring) != AuditServiceRegistration::Registered)
            return 1;
        const auto description = mddlog::core::AuditDescription::create({.action = "inventory.inspect"});
        const auto context     = mddlog::core::AuditContext::create({.target = "warehouse"});
        if (!description || !context)
            return 1;
        mddlog::core::AuditBinding audit(ring, *description, *context);
        if (!emit(audit))
            return 1;
        // Called by the host loop, including during producer inactivity.
        if ((*service)->poll().handedOff != 1) {
            const auto health = (*service)->health();
            std::println(std::cerr,
                         "Audit hand-off failed: delivery {}, storage {}",
                         std::to_underlying(health.delivery.lastIssue),
                         std::to_underlying(health.storage.lastIssue));
            return 1;
        }
        // Producers are quiescent here. The report keeps hand-off separate from confirmation.
        const auto stopped = (*service)->stop();
        if (!stopped.closed || stopped.health.delivery.pendingInRings != 0 || stopped.health.delivery.reportedLosses != 0)
            return 1;
        const auto stored = std::ranges::find(stopped.health.storage.streams, stream, &StreamStorageHealth::streamId);
        if (stored == stopped.health.storage.streams.end() || stored->state != StreamStorageState::Closed)
            return 1;
        std::println("Admitted and written: 1; durable position: {} (unqualified deployment)", stored->durablePosition);
    }
    files.access = FileAccess::ReadOnly;
    auto reader  = FileStorageMedium::create(files);
    if (!reader)
        return 1;
    const auto         stored = readStoredStream(**reader, stream);
    AuditChainVerifier verifier(stream);
    if (!stored.complete() || stored.resourceIssue() != AuditResourceIssue::None || stored.records().size() != 1 || stored.trailingBytesOfLastSegment() != 0)
        return 1;
    for (const auto& record : stored.records()) {
        if (verifier.check(record.bytes, record.digest) != ChainFinding::Ok)
            return 1;
    }
    std::println("Reopened read-only: 1 record; chain verified. Independent witness remains required.");
    return 0;
}
