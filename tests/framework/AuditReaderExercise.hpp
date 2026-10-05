/** @brief Bounded mutation harness for canonical, frame, ledger, recovery and log readers. */
#ifndef MDDLOG_TESTS_FRAMEWORK_AUDITREADEREXERCISE_HPP
#define MDDLOG_TESTS_FRAMEWORK_AUDITREADEREXERCISE_HPP

namespace mddlog::spec {

inline void requireReaderInvariant(bool condition, const std::source_location location = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error(std::format("audit invariant failed at {}:{}", location.file_name(), location.line()));
}

inline void exerciseReaderInput(std::span<const std::uint8_t> bytes) {
    using namespace mddlog::adapter;
    const auto             decoded = decodeCanonical(bytes);
    const std::string_view stream  = decoded.status == CanonicalReadStatus::Ok ? decoded.record.streamId : "fuzz/producer";
    if (decoded.status == CanonicalReadStatus::Ok) {
        requireReaderInvariant(decoded.record.sequence != 0);
        LedgerChecker ledger{decoded.record.streamId};
        (void)ledger.check(decoded.record);
        AuditChainVerifier chain{decoded.record.streamId};
        const auto         finding = chain.check(bytes, chainDigest(bytes, chainInitialValue));
        requireReaderInvariant((finding == ChainFinding::Ok) == (decoded.record.sequence == 1));
    }

    const auto scan = scanSegment(bytes);
    requireReaderInvariant(scan.validEnd <= bytes.size() && scan.trailingBytes == bytes.size() - scan.validEnd);
    std::size_t previous = 0;
    for (const auto& record : scan.records) {
        requireReaderInvariant(record.frameOffset > previous && record.frameOffset < scan.validEnd);
        requireReaderInvariant(record.canonical.size() <= framePayloadMax);
        previous = record.frameOffset;
        (void)decodeCanonical(record.canonical);
    }

    // A valid enclosing frame reaches semantic readers even when mutations invalidate a CRC.
    auto enclosed = encodeSegmentOpening(stream, 0, 1);
    if (bytes.size() <= framePayloadMax - sha256DigestSize) {
        const auto frame = encodeRecordFrame(bytes, chainDigest(bytes, chainInitialValue));
        enclosed.insert(enclosed.end(), frame.begin(), frame.end());
    }
    InMemoryStorageMedium medium{4};
    requireReaderInvariant(medium.open({.streamId = stream, .firstSequence = 1, .bytes = bytes}).status == OpenStatus::Opened);
    requireReaderInvariant(medium.open({.streamId = stream, .segmentIndex = 1, .firstSequence = 1, .bytes = enclosed}).status == OpenStatus::Opened);
    const auto analysis = LogAnalysis::read(medium);
    requireReaderInvariant(analysis.image().streams().size() <= 2);
    InMemoryAnchorProvider provider{"fuzz/witness"};
    RetainedPosition       retained;
    LogVerifier            verifier{medium, provider, retained};
    const auto             report = verifier.verify();
    for (const auto& item : report.streams)
        requireReaderInvariant(item.report.verdict != Verdict::Anchored);
    StorageConfig config;
    config.segmentCount       = 4;
    config.segmentSize        = 131072;
    config.maxProducerStreams = 2;
    (void)PersistingAuditSink::create(medium, config);
}

}  // namespace mddlog::spec
#endif
