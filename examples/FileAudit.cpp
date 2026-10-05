/** @brief File audit composition, explicit admission and offline verification after reopening (#114). */
import std;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditdrain;
import mddlog.core.auditbinding;

namespace {
using namespace mddlog::adapter;
constexpr std::size_t segmentBytes = 2048;
constexpr std::size_t segmentCount = 8;

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
        auto sink                  = PersistingAuditSink::create(medium, storage);
        if (!sink)
            return 1;
        if (const auto previous = medium.segments(); !previous || !previous->empty()) {
            std::println(std::cerr, "Use an empty directory for this demonstration; archive recovery belongs to the host lifecycle.");
            return 1;
        }
        mddlog::core::AuditRing<4> ring(stream);
        AuditSinkAdapter           consumer;
        if (consumer.addRing(ring) != AuditRingRegistration::Registered)
            return 1;
        consumer.setSink(*sink);
        const auto                 description = mddlog::core::AuditDescription::create({.action = "inventory.inspect"});
        const auto                 context     = mddlog::core::AuditContext::create({.target = "warehouse"});
        mddlog::core::AuditBinding audit(ring, *description, *context);
        if (!emit(audit))
            return 1;
        if (consumer.drainOnce().handedOff != 1)
            return 1;
        (*sink)->flush();
        std::println("Admitted and written: 1; durable position: {} (unqualified deployment)", (*sink)->durablePosition(stream));
    }
    files.access = FileAccess::ReadOnly;
    auto reader  = FileStorageMedium::create(files);
    if (!reader)
        return 1;
    const auto         stored = readStoredStream(**reader, stream);
    AuditChainVerifier verifier(stream);
    if (stored.records().size() != 1 || stored.trailingBytesOfLastSegment() != 0)
        return 1;
    for (const auto& record : stored.records()) {
        if (verifier.check(record.bytes, record.digest) != ChainFinding::Ok)
            return 1;
    }
    std::println("Reopened read-only: 1 record; chain verified. Independent witness remains required.");
    return 0;
}
