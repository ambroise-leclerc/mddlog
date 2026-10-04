/**
 * @brief Test doubles and builders for the ADR-004 Decision 10 specs: a sink over an in-memory medium with a ledger and a provider, and a forge that writes
 * arbitrary chained streams straight to a medium.
 *
 * Include after `import std;` and `import mddlog.adapter.auditlogverifier;` `import mddlog.adapter.auditstore;`.
 */
#ifndef MDDLOG_TESTS_FRAMEWORK_AUDITLOGRIG_HPP
#define MDDLOG_TESTS_FRAMEWORK_AUDITLOGRIG_HPP

namespace mddlog::spec::auditlog {

using namespace mddlog::adapter;
using mddlog::core::AuditEvent;
using mddlog::core::RawTime;

inline constexpr std::size_t segmentBytes = 2048;

[[nodiscard]] inline AuditEvent makeEvent(std::string_view stream, std::uint64_t sequence, std::size_t detailSize = 12) {
    AuditEvent  event;
    std::string detail = "step " + std::to_string(sequence);
    detail.resize(std::max(detail.size(), detailSize), 'x');
    core::AuditInput input;
    input.action = "therapy.rate.set";
    input.target = "pump/channel-A";
    input.detail = detail;
    (void)event.assign(input, stream, sequence);
    return event;
}

/** @brief Record frames of the given detail size that fit in one segment of the test configuration, after a 45-byte-ish opening. */
[[nodiscard]] inline std::uint64_t framesPerSegment(std::string_view stream, std::size_t detailSize) {
    const auto encoded = CanonicalRecord::encode(makeEvent(stream, 1, detailSize));
    return (segmentBytes - encodeSegmentOpening(stream, 0, 1).size()) / recordFrameSize(encoded.value_or(CanonicalRecord{}).size());
}

/** @brief H_k of a fresh chain over `makeEvent(stream, 1..k, detailSize)`. */
[[nodiscard]] inline Sha256Digest chainDigestAt(std::string_view stream, std::uint64_t position, std::size_t detailSize = 12) {
    AuditChain   chain{stream};
    Sha256Digest head = chainInitialValue;
    for (std::uint64_t k = 1; k <= position; ++k)
        head = chain.append(makeEvent(stream, k, detailSize))->digest;
    return head;
}

/** @brief An anchor provider that can fail only its `retire`, to stop an operation between a removal and its retirement. It counts the calls it receives. */
class FlakyProvider final : public AnchorProvider {
public:
    explicit FlakyProvider(std::string identity) : inner(std::move(identity)) {}

    [[nodiscard]] AdvanceAnswer advance(const AnchorClaim& claim) override {
        ++advances;
        return inner.advance(claim);
    }
    [[nodiscard]] RetireAnswer retire(std::string_view streamId, std::uint64_t position) override {
        ++retires;
        if (failRetire)
            return ProviderUnavailable{};
        return inner.retire(streamId, position);
    }
    [[nodiscard]] LatestAnswer latest(std::string_view streamId) override {
        ++latests;
        return inner.latest(streamId);
    }
    [[nodiscard]] StreamsAnswer streams() override {
        return inner.streams();
    }

    InMemoryAnchorProvider inner;
    bool                   failRetire = false;
    std::size_t            advances   = 0;
    std::size_t            retires    = 0;
    std::size_t            latests    = 0;
};

/**
 * @brief A sink over an in-memory medium, with a provider and a recorded clock. Heap-allocated: the sink's callbacks point into it.
 *
 * Each `start()` is a new adapter start on the same medium: a new ledger identity `ledger/<n>`, like a restart (ADR-002 Decision 5).
 */
struct Rig {
    explicit Rig(std::size_t capacity = 24, bool eligible = true) : medium(capacity, eligible), segmentCount(capacity) {}

    [[nodiscard]] StorageConfig config(std::size_t recordBound = 1) const {
        StorageConfig out;
        out.segmentSize        = segmentBytes;
        out.segmentCount       = segmentCount;
        out.maxProducerStreams = 2;
        out.sync.recordBound   = recordBound;
        return out;
    }
    [[nodiscard]] std::string ledgerId() const {
        return "ledger/" + std::to_string(session);
    }

    /** @brief Start a session. `declared.ledger` and `declared.provider` are filled in unless the caller set them. */
    [[nodiscard]] bool start(StorageConfig declared, bool withProvider = true) {
        ++session;
        declared.clock = [this] {
            return now;
        };
        declared.reportLoss = [this](std::uint64_t count) {
            losses.push_back(count);
        };
        if (!declared.ledger.has_value()) {
            LedgerConfig ledgerConfig;
            ledgerConfig.streamId = ledgerId();
            ledgerConfig.time     = [this] {
                return RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::seconds{1'000 + wallSeconds++}});
            };
            declared.ledger = ledgerConfig;
        }
        if (withProvider && declared.provider == nullptr)
            declared.provider = &provider;
        auto made = PersistingAuditSink::create(medium, std::move(declared));
        if (!made)
            return false;
        sink = *made;
        return true;
    }
    [[nodiscard]] bool start(std::size_t recordBound = 1, bool withProvider = true) {
        return start(config(recordBound), withProvider);
    }
    /** @brief A restart after a power loss: the medium keeps what 9.2 allows, and the old sink is gone. */
    void powerLoss(InMemoryStorageMedium::Unconfirmed policy = InMemoryStorageMedium::Unconfirmed::Dropped) {
        sink.reset();
        medium.restart(policy);
    }
    /** @brief Hand `count` events of `stream` to the sink, from sequence `from`. */
    [[nodiscard]] std::size_t feed(std::string_view stream, std::uint64_t from, std::uint64_t count, std::size_t detailSize = 12) const {
        std::size_t accepted = 0;
        for (std::uint64_t k = from; k < from + count; ++k)
            accepted += sink->accept(makeEvent(stream, k, detailSize)) ? 1U : 0U;
        return accepted;
    }

    InMemoryStorageMedium                 medium;
    FlakyProvider                         provider{"witness-1"};
    std::shared_ptr<PersistingAuditSink>  sink;
    std::chrono::steady_clock::time_point now;
    std::vector<std::uint64_t>            losses;
    std::size_t                           segmentCount;
    std::size_t                           session     = 0;
    std::int64_t                          wallSeconds = 0;
};

[[nodiscard]] inline std::unique_ptr<Rig> makeRig(std::size_t capacity = 24, bool eligible = true) {
    return std::make_unique<Rig>(capacity, eligible);
}

/** @brief A reader's full report, with a fresh retained position unless one is given. */
[[nodiscard]] inline LogReport readLog(Rig& rig, RetainedPosition& retained) {
    LogVerifier verifier{rig.medium, rig.provider, retained};
    return verifier.verify();
}
[[nodiscard]] inline LogReport readLog(Rig& rig) {
    RetainedPosition retained;
    return readLog(rig, retained);
}

/** @brief The stream's segments as the medium holds them now. */
[[nodiscard]] inline std::vector<SegmentImage> segmentsOf(StorageMedium& medium, std::string_view stream) {
    const LogImage image = LogImage::read(medium);
    const auto*    found = image.find(stream);
    return found == nullptr ? std::vector<SegmentImage>{} : found->segments;
}

/** @brief Records of a stream the medium holds, by sequence: the first and the last, or 0 and 0. */
struct Held {
    std::uint64_t first = 0;
    std::uint64_t last  = 0;
    std::size_t   count = 0;
};
[[nodiscard]] inline Held heldOf(StorageMedium& medium, std::string_view stream) {
    const auto segments = segmentsOf(medium, stream);
    Held       out;
    for (const auto& segment : segments) {
        if (segment.recordCount == 0)
            continue;
        if (out.count == 0)
            out.first = segment.firstSequence;
        out.last   = segment.lastSequence();
        out.count += segment.recordCount;
    }
    return out;
}

/** @brief The ledger records the medium holds for every ledger, as actions in order, "ledger/1: mddlog.ledger.origin", ... */
[[nodiscard]] inline std::vector<std::string> ledgerActions(StorageMedium& medium) {
    const LogAnalysis        log = LogAnalysis::read(medium);
    std::vector<std::string> out;
    for (const LedgerImage* item : log.ledgersOldestFirst()) {
        const StreamImage* stream = log.image().find(item->id);
        for (const StoredRecord& record : stream->records) {
            const auto read = decodeCanonical(record.bytes);
            out.push_back(item->id + ": " + std::string{read.record.action} + " " + std::string{read.record.target});
        }
    }
    return out;
}

[[nodiscard]] inline AuditEvent ledgerEvent(const LedgerEntry& entry, std::string_view ledgerId, std::uint64_t sequence) {
    return buildLedgerEvent(entry, ledgerId, sequence, RawTime::unavailable()).value_or(AuditEvent{});
}

/**
 * @brief Write one stream instance's events straight to a medium as one new segment, chained from H_0. Records before `from` are chained but not stored,
 * which is what a stream looks like after a rotation. The header names the first stored sequence.
 */
inline SegmentRef forgeSegment(StorageMedium&              medium,
                               std::string_view            stream,
                               std::span<const AuditEvent> events,
                               std::size_t                 from          = 0,
                               std::uint32_t               segmentIndex  = 0,
                               std::optional<std::size_t>  corruptDigest = std::nullopt,
                               std::size_t                 to            = std::numeric_limits<std::size_t>::max()) {
    AuditChain chain{stream};
    to                 = std::min(to, events.size());
    const auto opening = encodeSegmentOpening(stream, segmentIndex, events[from].sequence());
    const auto opened  = medium.open({.streamId = stream, .segmentIndex = segmentIndex, .firstSequence = events[from].sequence(), .bytes = opening});
    for (std::size_t i = 0; i < to; ++i) {
        const auto chained = chain.append(events[i]);
        if (i >= from) {
            Sha256Digest stored = chained->digest;
            if (corruptDigest == i)
                stored[0] = static_cast<std::uint8_t>(stored[0] ^ 0x01U);  // a rewrite that recomputed the frame's check but not the chain
            const auto frame = encodeRecordFrame(chained->canonical.bytes(), stored);
            (void)medium.append(opened.segment, frame);
        }
    }
    return opened.segment;
}

/** @brief Copy every segment of one medium onto another, byte for byte, as a restore or a merge of two logs would. */
inline void copySegments(StorageMedium& from, StorageMedium& to) {
    for (const auto& info : from.segments().value_or(std::vector<SegmentInfo>{})) {
        const auto bytes = from.read(info.segment, 0, info.size).value_or(std::vector<std::uint8_t>{});
        const auto scan  = scanSegment(bytes);
        if (!scan.header.has_value())
            continue;
        const SegmentHeader& header = *scan.header;
        const auto           open   = encodeSegmentOpening(header.streamId, header.segmentIndex, header.firstSequence);
        const auto made = to.open({.streamId = header.streamId, .segmentIndex = header.segmentIndex, .firstSequence = header.firstSequence, .bytes = open});
        (void)to.append(made.segment, std::span{bytes}.subspan(open.size()));
    }
}

}  // namespace mddlog::spec::auditlog

#endif
