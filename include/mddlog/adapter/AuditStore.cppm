/**
 * @brief Persisting audit sink: segments, durable confirmation, storage health, and with a ledger, restart, rotation and retention (ADR-004 Decisions 9.3, 9.5,
 * 9.6, 10.2 to 10.5). Adapter zone only.
 */

export module mddlog.adapter.auditstore;

import std;
export import mddlog.sinks.auditsink;
export import mddlog.adapter.auditchain;
export import mddlog.adapter.auditlayout;
export import mddlog.adapter.auditmedium;
export import mddlog.adapter.auditanchor;
export import mddlog.adapter.auditverifier;
export import mddlog.adapter.auditledger;
export import mddlog.adapter.auditlog;

export namespace mddlog::adapter {

/** @brief Where a stream instance stands on the medium (9.6). */
enum class StreamStorageState : std::uint8_t {
    /** @brief Appending, and confirming per the sync policy. */
    Persisting,
    /** @brief It needs a segment and only the ledger's reserve is free. Nothing is appended, nothing is skipped; events stay in the ring (9.6). */
    Full,
    /** @brief The instance ended on a storage failure or an integrity fault. Nothing more is written to it (9.6). */
    Failed,
    /** @brief Orderly close: its last record is confirmed and the ledger recorded `close`. Nothing more is accepted for it (10.2). */
    Closed
};

/** @brief The cause the health signal names last (9.6). */
enum class StorageIssue : std::uint8_t {
    None,
    Full,
    OpenFailed,
    AppendFailed,
    SyncFailed,
    NoSpace,
    DuplicateMismatch,
    SequenceOutOfOrder,
    NotEncodable,
    MediumUnreadable,
    /** @brief Recovery or a provider check found a suspect stream or ledger (10.3, 7.3). The records are never repaired. */
    IntegrityFault
};

/** @brief When a sync is issued, besides the one before a rotation and the one on orderly close (9.3). */
struct SyncPolicy {
    /** @brief Sync once this many appended records are unconfirmed; 1 is a sync per record. Zero is invalid. */
    std::size_t recordBound = 1;
    /** @brief Also sync once the oldest unconfirmed record is this old. Checked at each accept() and each tick(). Must be positive. */
    std::optional<std::chrono::nanoseconds> ageBound;
};

/** @brief The ledger of one adapter start (10.1): its stream identity and the host's clock for its records. */
struct LedgerConfig {
    /** @brief This start's ledger stream identity. The host guarantees it is new, under the uniqueness obligations of ADR-002 Decision 5. */
    std::string streamId;
    /** @brief Host-supplied time stamped on ledger records; unavailable when empty. */
    std::function<core::RawTime()> time;
};

/**
 * @brief What relieve() is allowed to remove (10.4, 10.5). Nothing is removed without a declaration: trimPrefix() and removeStream() are the host's explicit
 * calls, and these flags are the integrator's standing permission for a full producer stream.
 */
struct RetentionPolicy {
    /** @brief Trim the prefix of a kept stream, up to its anchor, one segment at a time (10.4). */
    bool rotate = false;
    /** @brief Remove whole ended streams, and ledgers that satisfy 10.5, oldest first, after rotation (10.5). */
    bool removeEnded = false;
};

/** @brief What the integrator declares about the medium and the sink (9.1, 9.3, 9.6). */
struct StorageConfig {
    /** @brief Bytes in one segment, preamble and frames included. */
    std::size_t segmentSize = 0;
    /** @brief N: segments the medium can hold. */
    std::size_t segmentCount = 0;
    /** @brief S: producer streams open at once. A stream beyond it is refused. */
    std::size_t maxProducerStreams = 0;
    SyncPolicy  sync;
    /** @brief Clock for the age bound; steady_clock when empty. */
    std::function<std::chrono::steady_clock::time_point()> clock;
    /** @brief Where events accepted but never confirmed are reported as losses after admission when an instance fails. Typically AuditSinkAdapter::reportLoss.
     */
    std::function<void(std::uint64_t)> reportLoss;
    /** @brief The ledger of this start. Without it, restart, rotation and retention are not available and the sink behaves as before (10.1). */
    std::optional<LedgerConfig> ledger;
    /**
     * @brief The anchor provider (7.2), or null: no anchor is ever advanced or retired, and rotation is not bounded by an anchor (10.4). Must outlive the sink.
     */
    AnchorProvider* provider = nullptr;
    RetentionPolicy retention;
};

enum class StorageConfigError : std::uint8_t {
    /** @brief A segment cannot hold its opening and the largest record frame. */
    SegmentTooSmall,
    NoProducerStreams,
    InvalidSyncPolicy,
    /** @brief The medium cannot hold the ledger's reserve and one more segment (9.6). */
    MediumTooSmall,
    /** @brief The ledger's stream identity is one AuditEvent rejects. */
    InvalidLedgerStream,
    /** @brief The medium already holds a stream instance with the ledger's identity: a restart starts a new ledger (10.1). */
    LedgerIdentityInUse
};

/**
 * @brief Segments kept free for the ledger: the number that holds `N + S + 3` record frames of 813 bytes, at `⌊(segmentSize − 125) / 813⌋` frames per
 * segment, plus one segment (9.6). Zero when the segment is too small to hold one frame.
 */
[[nodiscard]] constexpr std::size_t ledgerReserveSegments(std::size_t segmentSize, std::size_t segmentCount, std::size_t maxProducerStreams) noexcept {
    if (segmentSize < maxSegmentOpeningSize + maxRecordFrameSize)
        return 0;
    const std::size_t perSegment = (segmentSize - maxSegmentOpeningSize) / maxRecordFrameSize;
    const std::size_t frames     = segmentCount + maxProducerStreams + 3;
    return ((frames + perSegment - 1) / perSegment) + 1;
}

/** @brief What the startup check found, and where (9.6, "Recovery findings"). */
enum class RecoveryFindingKind : std::uint8_t {
    UnreadableSegment,
    NoValidPreamble,
    UnknownLayoutVersion,
    NoValidHeader,
    DuplicateSegmentIndex,
    SegmentIndexGap,
    /** @brief In the last segment of an earlier instance: bytes after the last valid frame, from `offset` for `length`. Never parsed. */
    TrailingBytes
};

struct RecoveryFinding {
    RecoveryFindingKind kind    = RecoveryFindingKind::UnreadableSegment;
    SegmentRef          segment = 0;
    std::string         streamId;
    std::uint32_t       segmentIndex = 0;
    std::uint64_t       offset       = 0;
    std::uint64_t       length       = 0;
};

/**
 * @brief Exactly what the bounded startup check covers, and nothing more (9.6): every segment's preamble and header frame, per instance the duplicate and
 * missing segment indices, and a full frame scan of the last segment of each instance. It reads no record frame of an earlier segment, recomputes no
 * digest, and does not check that `firstSequence` follows on or that a record's stream matches its header's. Those belong to the verifier.
 */
struct RecoveryReport {
    /** @brief False when the medium could not list its segments: the check covered nothing. */
    bool                         mediumReadable   = true;
    std::size_t                  segmentsExamined = 0;
    std::vector<RecoveryFinding> findings;
    /** @brief Identities of the stream instances the medium already holds. */
    std::vector<std::string> streamsHeld;
};

[[nodiscard]] inline RecoveryFinding unlocatedFinding(RecoveryFindingKind kind, SegmentRef segment) {
    RecoveryFinding finding;
    finding.kind    = kind;
    finding.segment = segment;
    return finding;
}

[[nodiscard]] inline RecoveryReport checkMediumAtStart(StorageMedium& medium) {
    RecoveryReport report;
    const auto     listing = medium.segments();
    if (!listing) {
        report.mediumReadable = false;
        return report;
    }
    struct Held {
        SegmentRef    segment = 0;
        std::uint64_t size    = 0;
        std::uint32_t index   = 0;
    };
    std::map<std::string, std::vector<Held>, std::less<>> instances;
    for (const auto& info : *listing) {
        ++report.segmentsExamined;
        const auto prefix = medium.read(info.segment, 0, std::min<std::uint64_t>(info.size, maxSegmentOpeningSize));
        if (!prefix) {
            report.findings.push_back(unlocatedFinding(RecoveryFindingKind::UnreadableSegment, info.segment));
            continue;
        }
        const SegmentScan scan = scanSegment(*prefix);
        switch (scan.status) {
            case SegmentStatus::NoValidPreamble:
                report.findings.push_back(unlocatedFinding(RecoveryFindingKind::NoValidPreamble, info.segment));
                break;
            case SegmentStatus::UnknownLayoutVersion:
                report.findings.push_back(unlocatedFinding(RecoveryFindingKind::UnknownLayoutVersion, info.segment));
                break;
            case SegmentStatus::NoValidHeader:
                report.findings.push_back(unlocatedFinding(RecoveryFindingKind::NoValidHeader, info.segment));
                break;
            case SegmentStatus::Readable:
                if (scan.header)
                    instances[scan.header->streamId].push_back({.segment = info.segment, .size = info.size, .index = scan.header->segmentIndex});
                break;
        }
    }
    for (auto& [streamId, held] : instances) {
        report.streamsHeld.push_back(streamId);
        std::ranges::sort(held, {}, &Held::index);
        for (std::size_t i = 1; i < held.size(); ++i) {
            if (held[i].index == held[i - 1].index) {
                report.findings.push_back(
                    {.kind = RecoveryFindingKind::DuplicateSegmentIndex, .segment = held[i].segment, .streamId = streamId, .segmentIndex = held[i].index});
            } else if (held[i].index != held[i - 1].index + 1) {
                report.findings.push_back(
                    {.kind = RecoveryFindingKind::SegmentIndexGap, .segment = held[i].segment, .streamId = streamId, .segmentIndex = held[i].index});
            }
        }
        const Held& last = held.back();
        const auto  all  = medium.read(last.segment, 0, last.size);
        if (!all) {
            report.findings.push_back(
                {.kind = RecoveryFindingKind::UnreadableSegment, .segment = last.segment, .streamId = streamId, .segmentIndex = last.index});
            continue;
        }
        const SegmentScan scan = scanSegment(*all);
        if (scan.trailingBytes != 0) {
            report.findings.push_back({.kind         = RecoveryFindingKind::TrailingBytes,
                                       .segment      = last.segment,
                                       .streamId     = streamId,
                                       .segmentIndex = last.index,
                                       .offset       = scan.validEnd,
                                       .length       = scan.trailingBytes});
        }
    }
    return report;
}

/**
 * @brief The records of one stream instance as a reader holds them, ready for AnchorVerifier, with the segment structure the verifier needs. Owns the bytes
 * the records point into.
 *
 * Pass `records()` and `layout()` together: the layout carries what a bare list of records loses, namely the segment boundaries and headers (9.5), a segment
 * of an unknown layout version (9.4) and a segment that could not be read.
 */
class StoredStream {
public:
    StoredStream()                                   = default;
    StoredStream(const StoredStream&)                = delete;
    StoredStream& operator=(const StoredStream&)     = delete;
    StoredStream(StoredStream&&) noexcept            = default;
    StoredStream& operator=(StoredStream&&) noexcept = default;
    ~StoredStream()                                  = default;

    /** @brief Records of every readable segment of the instance, in segment order, then storage order. */
    [[nodiscard]] const std::vector<StoredRecord>& records() const noexcept {
        return stored;
    }
    /** @brief The segment boundaries, an unknown layout version and unreadable segments, for AnchorVerifier::verify(). */
    [[nodiscard]] const StoredLayout& layout() const noexcept {
        return structure;
    }
    /** @brief Indices of the segments that held a valid header for this instance, in order. */
    [[nodiscard]] const std::vector<std::uint32_t>& segmentIndices() const noexcept {
        return indices;
    }
    /** @brief Bytes after the last valid frame of the last segment: the trace of a cut, never parsed. */
    [[nodiscard]] std::size_t trailingBytesOfLastSegment() const noexcept {
        return trailing;
    }
    /** @brief Segments with no valid preamble or header, which no stream can claim: the usual trace of a cut during open (9.5). They do not stop a verdict. */
    [[nodiscard]] std::size_t segmentsWithoutHeader() const noexcept {
        return withoutHeader;
    }
    /** @brief False when the medium could not be listed, a segment could not be read, or a segment is in an unknown layout version: the records may not be the
     * stream. */
    [[nodiscard]] bool complete() const noexcept {
        return !structure.unreadable && !structure.unknownLayoutVersion.has_value();
    }

private:
    friend StoredStream readStoredStream(StorageMedium&, std::string_view);

    std::vector<std::vector<std::uint8_t>> buffers;
    std::vector<StoredRecord>              stored;
    std::vector<std::uint32_t>             indices;
    StoredLayout                           structure;
    std::size_t                            trailing      = 0;
    std::size_t                            withoutHeader = 0;
};

/**
 * @brief Read back the records of `streamId` from every segment whose header names it. Records are not verified: that is AnchorVerifier's job.
 *
 * A segment in a layout version this reader does not know has no header it can read, so it cannot be tied to a stream: it is reported for every stream, and
 * none of them can be verified (9.4).
 */
[[nodiscard]] inline StoredStream readStoredStream(StorageMedium& medium, std::string_view streamId) {
    StoredStream out;
    const auto   listing = medium.segments();
    if (!listing) {
        out.structure.unreadable = true;
        return out;
    }
    struct Found {
        std::uint32_t index         = 0;
        std::uint64_t firstSequence = 0;
        std::size_t   buffer        = 0;
    };
    std::vector<Found> found;
    for (const auto& info : *listing) {
        auto bytes = medium.read(info.segment, 0, info.size);
        if (!bytes) {
            out.structure.unreadable = true;
            continue;
        }
        const SegmentScan scan = scanSegment(*bytes);
        switch (scan.status) {
            case SegmentStatus::UnknownLayoutVersion:
                if (!out.structure.unknownLayoutVersion)
                    out.structure.unknownLayoutVersion = scan.layoutVersion;
                break;
            case SegmentStatus::NoValidPreamble:
            case SegmentStatus::NoValidHeader:
                ++out.withoutHeader;
                break;
            case SegmentStatus::Readable:
                if (scan.header && scan.header->streamId == streamId) {
                    found.push_back({.index = scan.header->segmentIndex, .firstSequence = scan.header->firstSequence, .buffer = out.buffers.size()});
                    out.buffers.push_back(std::move(*bytes));
                }
                break;
        }
    }
    std::ranges::sort(found, {}, &Found::index);
    for (const auto& item : found) {
        const SegmentScan scan = scanSegment(out.buffers[item.buffer]);
        out.indices.push_back(item.index);
        out.structure.segments.push_back({.segmentIndex = item.index, .firstSequence = item.firstSequence, .recordCount = scan.records.size()});
        for (const auto& record : scan.records)
            out.stored.push_back({.bytes = record.canonical, .digest = record.digest});
        out.trailing = scan.trailingBytes;
    }
    return out;
}

/** @brief One stream instance's storage state and durable position, as the health signal publishes it (9.6). */
struct StreamStorageHealth {
    std::string        streamId;
    StreamStorageState state = StreamStorageState::Persisting;
    StorageIssue       cause = StorageIssue::None;
    /** @brief Highest sequence a sync confirmed durable, or 0. It only increases (9.3). */
    std::uint64_t durablePosition = 0;
    /** @brief Highest sequence appended. Written, not confirmed. */
    std::uint64_t appendedPosition = 0;
    std::size_t   segmentsOpened   = 0;
};

/** @brief Counters across instances (9.6). Values read together are not a single synchronized snapshot. */
struct StorageCounters {
    std::uint64_t fullEntries        = 0;
    std::uint64_t openFailures       = 0;
    std::uint64_t appendFailures     = 0;
    std::uint64_t syncFailures       = 0;
    std::uint64_t noSpace            = 0;
    std::uint64_t duplicateMismatch  = 0;
    std::uint64_t sequenceOutOfOrder = 0;
    std::uint64_t notEncodable       = 0;
    std::uint64_t mediumUnreadable   = 0;
    /** @brief Syncs a non-eligible medium answered Unsupported to. Not a failure. */
    std::uint64_t syncUnsupported = 0;
    /** @brief Events refused for an identity AuditEvent rejects. */
    std::uint64_t invalidStream = 0;
    /** @brief Events refused because S producer streams were already open. */
    std::uint64_t streamLimitRefused = 0;
    /**
     * @brief Events refused because the medium already holds an instance of that identity, or, with a ledger, because a ledger in the log names it: a restart
     * must start a new instance (ADR-002 Decision 5), and a removed stream's trims would otherwise be read as the new instance's.
     */
    std::uint64_t streamIdentityInUse = 0;
    /** @brief Events accepted but never confirmed when an instance failed, reported as losses after admission (9.6). */
    std::uint64_t notDurableAtFailure = 0;
    /** @brief Findings of the startup check (9.6). */
    std::uint64_t recoveryFindings = 0;
    /** @brief Events refused for an action under the reserved ledger namespace (10.1). Producer admission refuses them first. */
    std::uint64_t reservedActionRefused = 0;
    /** @brief Events refused because the ledger failed: nothing may be opened or stored without it (10.2). */
    std::uint64_t ledgerRefused = 0;
    std::uint64_t ledgerRecords = 0;
    /** @brief Trim records written, whole segments reclaimed, and whole streams removed (10.4, 10.5). */
    std::uint64_t trimsRecorded     = 0;
    std::uint64_t segmentsReclaimed = 0;
    std::uint64_t streamsRemoved    = 0;
    /** @brief Retirements the provider accepted (10.5). */
    std::uint64_t retirements = 0;
    /** @brief Retention operations that removed nothing, whatever the reason (10.4). RetentionResult::trimmedThrough says whether a trim was still recorded. */
    std::uint64_t retentionRefused = 0;
    std::uint64_t anchorsAccepted  = 0;
    /** @brief Refusals of a fresh claim, which mean the log or the provider diverged (7.3). */
    std::uint64_t anchorsRefused     = 0;
    std::uint64_t anchorsUnavailable = 0;
    /** @brief Suspect streams or ledgers found at start or by a provider check (10.3, 7.3). */
    std::uint64_t integrityFaults = 0;
};

/** @brief Why a retention operation did what it did (10.4, 10.5). Only Trimmed and Removed removed anything. */
enum class RetentionOutcome : std::uint8_t {
    /** @brief The trim record was confirmed and the segments were reclaimed. */
    Trimmed,
    /** @brief The whole stream was trimmed and reclaimed. */
    Removed,
    /** @brief The sink has no ledger, or its ledger failed: nothing may be removed without a recorded trim. */
    NoLedger,
    UnknownStream,
    /** @brief The target is a ledger, which is not rotated (10.5), or the current ledger itself. */
    NotRotatable,
    /** @brief The stream is still being written, or ended on a failure this session. */
    StreamNotEnded,
    /** @brief Recovery found the stream inconsistent, a continuity check against the provider's anchor included: its records are never removed (10.3, 7.3), and
       a fault is reported. */
    StreamInconsistent,
    /** @brief A provider is configured and has accepted no anchor for the stream (10.4). */
    NoAnchor,
    ProviderUnavailable,
    /** @brief No segment ends at or before the bound (10.4), or nothing is left to trim. */
    NothingToTrim,
    /** @brief The trim record could not be durably confirmed, or the medium answers Unsupported so it never can: nothing was removed (9.3). */
    NotConfirmed,
    /**
     * @brief The ledger's anchor could not be advanced past the trim, so a retirement could not be authorized later (7.2, 10.5). The trim is recorded and
     * nothing was removed: a reader reports a removal that did not happen.
     */
    LedgerNotAnchored,
    /**
     * @brief The ledger rules of 10.5 are not met: a stream it opened remains, or no newer ledger cites it. Also while the ledger holds the highest trim of a
     * stream the log still holds, or of a removed stream whose retirement is still owed: removing it would erase the only record of that removal.
     */
    LedgerStillNeeded,
    MediumUnreadable,
    /** @brief The trim was confirmed but the medium did not reclaim every segment: the removal is interrupted (10.4). */
    ReclaimInterrupted
};

struct RetentionResult {
    RetentionOutcome outcome = RetentionOutcome::NothingToTrim;
    /** @brief The trim's position `q`, when a trim was recorded, even when nothing was then removed (NotConfirmed after the write, LedgerNotAnchored). */
    std::uint64_t trimmedThrough    = 0;
    std::size_t   segmentsReclaimed = 0;
    /** @brief The provider accepted the retirement of a whole stream. False when none was needed, or when it is still owed (10.5). */
    bool retired = false;
};

/** @brief What the ledger recorded at this start (10.2, 10.3). */
struct RestartReport {
    bool ledgerEnabled = false;
    /** @brief Record 1 and the `recovered` records are written; false when the ledger failed while writing them. */
    bool             startWritten = false;
    LedgerRecordKind firstRecord  = LedgerRecordKind::Origin;
    /** @brief The earlier ledger record 1 cites; empty for `origin`. */
    std::string                  predecessor;
    bool                         predecessorChecks = false;
    std::optional<std::uint64_t> predecessorPosition;
    std::size_t                  recoveredChecked = 0;
    std::size_t                  recoveredFailed  = 0;
    bool                         fork             = false;
    /** @brief Retirements owed since an earlier start, which this start completed (7.3, 10.5). */
    std::size_t                 retirementsCompleted = 0;
    std::vector<IntegrityFault> faults;
};

struct StorageHealthSnapshot {
    std::vector<StreamStorageHealth> streams;
    /** @brief The suspect streams and ledgers found at start and by provider checks. Nothing here is an audit event (9.6). */
    std::vector<IntegrityFault> integrity;
    /** @brief Free segments beyond the ledger's reserve, as last counted: what producer streams may still open. */
    std::uint64_t   freeSegmentsBeyondReserve = 0;
    std::uint64_t   reserveSegments           = 0;
    StorageCounters counters;
    StorageIssue    lastIssue = StorageIssue::None;
};

/**
 * @brief Audit sink that stores each stream instance's records as canonical frames in append-only segments and confirms them durably (ADR-004 9.1 to 9.6).
 *
 * `accept()` returning true is a hand-off to the medium's cache, never a durable confirmation: a record is durably confirmed only when a sync answered
 * Durable for an offset at or beyond its frame and every earlier record is confirmed (9.3). That is published per stream instance as the durable
 * position, and only a medium that answers Durable is eligible for it. A medium that answers Unsupported stores records and confirms none.
 *
 * It never reports a storage failure as an audit event or through any sink that writes to the failing storage (9.6): failures are counted in health().
 * accept(), tick(), flush(), close(), closeStream(), advanceAnchor(), trimPrefix(), removeStream() and relieve() belong to the one consumer thread that
 * drains the rings, like AuditSinkAdapter::drainOnce(). health(), durablePosition() and durableClaim() may be called from any thread; restart() is fixed at
 * creation. The medium and the anchor provider must outlive the sink.
 *
 * With a ledger (StorageConfig::ledger) the sink also records each stream before it stores anything of it, links this start to the previous one, closes
 * streams in order, and offers rotation and retention that write their trim record durably before any segment is reclaimed (ADR-004 Decision 10). It never
 * presents a restart as continuity: every start begins new stream instances.
 */
class PersistingAuditSink final : public sinks::AuditSink {
public:
    /**
     * @brief Validate the declared configuration against 9.6, run the startup check of the medium, and build the sink.
     *
     * Without a ledger it writes nothing. With one it also recovers chain state and writes the ledger's record 1 and its `recovered` records, each durably
     * confirmed before the next (10.2, 10.3); restart() says what was recorded, and a failure to write them is a failed ledger in health(), not an error here.
     */
    [[nodiscard]] static std::expected<std::shared_ptr<PersistingAuditSink>, StorageConfigError> create(StorageMedium& medium, StorageConfig config) {
        if (config.segmentSize < maxSegmentOpeningSize + maxRecordFrameSize)
            return std::unexpected{StorageConfigError::SegmentTooSmall};
        if (config.maxProducerStreams == 0)
            return std::unexpected{StorageConfigError::NoProducerStreams};
        if (config.sync.recordBound == 0 || (config.sync.ageBound && *config.sync.ageBound <= std::chrono::nanoseconds::zero()))
            return std::unexpected{StorageConfigError::InvalidSyncPolicy};
        const std::size_t reserve = ledgerReserveSegments(config.segmentSize, config.segmentCount, config.maxProducerStreams);
        if (config.segmentCount < reserve + 1)
            return std::unexpected{StorageConfigError::MediumTooSmall};
        if (config.ledger.has_value() && !core::AuditEvent::validStreamId(config.ledger->streamId))
            return std::unexpected{StorageConfigError::InvalidLedgerStream};
        if (!config.clock)
            config.clock = [] {
                return std::chrono::steady_clock::now();
            };
        auto recovered = checkMediumAtStart(medium);
        if (config.ledger.has_value() && std::ranges::find(recovered.streamsHeld, config.ledger->streamId) != recovered.streamsHeld.end())
            return std::unexpected{StorageConfigError::LedgerIdentityInUse};
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the constructor is private, so make_shared cannot reach it.
        std::shared_ptr<PersistingAuditSink> sink{new PersistingAuditSink(medium, std::move(config), reserve, std::move(recovered))};
        if (sink->config.ledger.has_value())
            sink->startLedger();
        return sink;
    }

    PersistingAuditSink(const PersistingAuditSink&)            = delete;
    PersistingAuditSink& operator=(const PersistingAuditSink&) = delete;
    PersistingAuditSink(PersistingAuditSink&&)                 = delete;
    PersistingAuditSink& operator=(PersistingAuditSink&&)      = delete;
    ~PersistingAuditSink() override                            = default;

    /** @brief What the startup check found. Fixed at creation. */
    [[nodiscard]] const RecoveryReport& recovery() const noexcept {
        return startup;
    }

    /**
     * @brief Hand the event off to the medium, or refuse it so it stays in the ring.
     *
     * True when the event is appended, or when it is an exact retry of the instance's last appended event (9.3). False for a stream that is full, failed,
     * invalid, over the stream limit or already held by the medium, and after close(). A failure that ends an instance returns false for the event it
     * struck, except a sync failure right after a successful append, which returns true: that event was already handed off and is counted as not durable.
     */
    [[nodiscard]] bool accept(const core::AuditEvent& event) override {
        if (closed)
            return false;
        if (core::isReservedAuditAction(event.action())) {
            // Producer admission refuses these first (10.1). A record built any other way never reaches the medium as a producer's: it could pass for a ledger.
            counters.reservedActionRefused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (ledger != nullptr && ledger->state == StreamStorageState::Failed) {
            counters.ledgerRefused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const std::string_view id = event.streamId();
        if (!core::AuditEvent::validStreamId(id)) {
            counters.invalidStream.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto it = streams.find(id);
        if (it == streams.end()) {
            if (std::ranges::binary_search(startup.streamsHeld, id) || std::ranges::binary_search(namedByLedgers, id, std::less<>{})
                || (ledger != nullptr && ledger->id == id)) {
                counters.streamIdentityInUse.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // S bounds the instances open at once: one that failed has ended and no longer counts (9.6).
            std::size_t open = 0;
            for (const auto& entry : streams)
                open += entry.second.state != StreamStorageState::Failed && entry.second.state != StreamStorageState::Closed ? 1U : 0U;
            if (open >= config.maxProducerStreams) {
                counters.streamLimitRefused.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // The ledger records the stream before any record of it is stored, and confirms that first (10.2).
            if (ledger != nullptr && !writeLedger(LedgerEntry::streamOpen(id))) {
                counters.ledgerRefused.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            it = streams.try_emplace(std::string{id}, std::string{id}).first;
            publish(it->second);
        }
        Stream& stream = it->second;
        if (stream.state == StreamStorageState::Failed || stream.state == StreamStorageState::Closed)
            return false;
        return append(stream, event);
    }

    /** @brief Issue the syncs whose age bound has passed. Call it periodically from the consumer thread, since an idle ring calls accept() no more. */
    void tick() {
        if (closed || !config.sync.ageBound)
            return;
        const auto now = config.clock();
        for (auto& entry : streams) {
            Stream& stream = entry.second;
            if (stream.state != StreamStorageState::Failed && stream.unsynced != 0 && now - stream.oldestUnsynced >= *config.sync.ageBound)
                (void)sync(stream);
        }
    }

    /**
     * @brief Free segments for full producer streams under the declared retention policy (10.4, 10.5). Call it from the consumer thread when a stream is full.
     *
     * Rotation first, one segment of one stream at a time, then, if declared, the removal of whole ended streams, oldest first. It stops as soon as a free
     * segment beyond the reserve exists. It removes nothing the policy does not allow, and never records of a stream recovery found inconsistent. Returns the
     * segments reclaimed.
     */
    std::size_t relieve() {
        if (ledger == nullptr || ledger->state == StreamStorageState::Failed || closed)
            return 0;
        const bool anyFull = std::ranges::any_of(streams, [](const auto& entry) {
            return entry.second.state == StreamStorageState::Full;
        });
        if (!anyFull || (!config.retention.rotate && !config.retention.removeEnded))
            return 0;
        std::size_t reclaimed = 0;
        const auto  roomLeft  = [&] {
            const auto beyond = refreshFree();
            return beyond.has_value() && *beyond > 0;
        };
        // One reading of the log and one recovery serve every attempt that changes nothing; an attempt that writes or removes anything invalidates them.
        std::optional<LogState> cache;
        const auto              attempt = [&](std::string_view target, bool whole) {
            const std::uint64_t   ledgerBefore = ledger->appended;
            const RetentionResult result       = retain(target, whole, &cache);
            if (result.segmentsReclaimed != 0 || ledger->appended != ledgerBefore)
                cache.reset();
            return result.segmentsReclaimed;
        };
        std::vector<std::string> order;
        {
            const LogState& state = current(cache);
            for (const LedgerImage* item : state.log.ledgersOldestFirst()) {
                for (const auto& stream : item->opened) {
                    if (state.log.image().find(stream) != nullptr && std::ranges::find(order, stream) == order.end())
                        order.push_back(stream);
                }
            }
        }
        if (config.retention.rotate) {
            for (const auto& stream : order) {
                if (roomLeft())
                    return reclaimed;
                reclaimed += attempt(stream, false);
            }
        }
        if (config.retention.removeEnded) {
            for (const auto& stream : order) {
                if (roomLeft())
                    return reclaimed;
                reclaimed += attempt(stream, true);
            }
            // A ledger goes only once every stream it opened has gone and a newer ledger cites it (10.5).
            std::vector<std::string> oldLedgers;
            for (const LedgerImage* item : current(cache).log.ledgersOldestFirst()) {
                if (item->id != ledger->id)
                    oldLedgers.push_back(item->id);
            }
            for (const auto& id : oldLedgers) {
                if (roomLeft())
                    return reclaimed;
                reclaimed += attempt(id, true);
            }
        }
        (void)roomLeft();
        return reclaimed;
    }

    /** @brief Sync every stream's open segment now, whatever the policy says. */
    void flush() {
        for (auto& entry : streams) {
            if (entry.second.state != StreamStorageState::Failed)
                (void)sync(entry.second);
        }
    }

    /**
     * @brief Orderly close: a final sync of every stream, then no more events are accepted (9.3).
     *
     * With a ledger, each stream is closed as closeStream() says, and `mddlog.ledger.close` is written only if every stream it opened was closed (10.2): a
     * stream that failed leaves the session without it, which a reader reports as an abrupt end. The ledger then attempts its own anchor.
     */
    void close() {
        if (closed)
            return;
        if (ledger == nullptr) {
            flush();
            closed = true;
            return;
        }
        bool everyStreamClosed = true;
        for (auto& entry : streams) {
            if (entry.second.state != StreamStorageState::Closed && !closeOne(entry.second))
                everyStreamClosed = false;
        }
        if (everyStreamClosed && ledger->state != StreamStorageState::Failed) {
            if (writeLedger(LedgerEntry::ledgerClose(ledger->id)))
                (void)advanceAnchor(*ledger);
        }
        closed = true;
    }

    /**
     * @brief Orderly close of one stream: its last record durably confirmed, the anchor attempt of 7.3, then the ledger's `close` record (10.2).
     *
     * False when the stream is unknown, failed, or its confirmation or the ledger write failed. Nothing more is accepted for a closed stream. It cites the last
     * appended position: on an eligible medium that is the durable position, and on a medium that answers Unsupported it is only what was handed off.
     */
    [[nodiscard]] bool closeStream(std::string_view streamId) {
        const auto found = streams.find(streamId);
        if (found == streams.end() || closed)
            return false;
        return closeOne(found->second);
    }

    /**
     * @brief Offer the stream's durable position to the anchor provider (7.3). True when the provider accepted a claim.
     *
     * Never above the durable position, never from state read back from storage, and never again for a position already accepted. Refusing a fresh claim as
     * `positionNotIncreasing` or `conflict` is an integrity fault, not a transient failure; unavailable is counted and the claim is not retried here.
     */
    bool advanceAnchor(std::string_view streamId) {
        const auto found = streams.find(streamId);
        if (found != streams.end())
            return advanceAnchor(found->second);
        if (ledger != nullptr && ledger->id == streamId)
            return advanceAnchor(*ledger);
        return false;
    }

    /** @brief What the ledger recorded at this start (10.2, 10.3). */
    [[nodiscard]] const RestartReport& restart() const noexcept {
        return restartInfo;
    }

    /**
     * @brief Rotation: remove a prefix of one kept stream (10.4).
     *
     * The adapter picks `q` as the last record of the latest whole segment that ends at or before the bound, writes `mddlog.stream.trim` with `q` and `H_q`,
     * waits for its durable confirmation, and only then reclaims the segments from the oldest forward. The bound is the stream's accepted anchor when a
     * provider is configured, after the continuity check of 7.3; the open segment and the last segment of the stream are never trimmed. Nothing is written when
     * nothing can be removed, and a stream recovery found inconsistent is never touched (10.3).
     */
    [[nodiscard]] RetentionResult trimPrefix(std::string_view streamId) {
        return retain(streamId, false, nullptr);
    }

    /**
     * @brief Retention of a whole ended stream, or of a whole ledger that 10.5 allows removing: a trim with `q` equal to its last position, then the removal,
     * then `retire` when the provider holds an anchor for it.
     *
     * With a provider the stream's `latest` decides: an anchor is retired after the removal, absent needs no retirement, unavailable writes and removes
     * nothing. Before removing a stream the provider anchors, the ledger's own anchor is advanced past the trim, so that a retirement an interruption leaves
     * owed can be authorized at the next start (7.2, condition 5).
     */
    [[nodiscard]] RetentionResult removeStream(std::string_view streamId) {
        return retain(streamId, true, nullptr);
    }

    [[nodiscard]] StorageHealthSnapshot health() const {
        StorageHealthSnapshot out;
        {
            const std::scoped_lock lock{publishedMutex};
            out.streams.reserve(published.size());
            for (const auto& entry : published)
                out.streams.push_back(entry.second);
        }
        {
            const std::scoped_lock lock{publishedMutex};
            out.integrity = integrityFaults;
        }
        out.freeSegmentsBeyondReserve = freeBeyondReserve.load(std::memory_order_relaxed);
        out.reserveSegments           = reserve;
        out.counters                  = counterSnapshot();
        out.lastIssue                 = lastIssue.load(std::memory_order_relaxed);
        return out;
    }

    /** @brief The durable position of a stream instance, or 0 when it is unknown or nothing is confirmed (9.3). */
    [[nodiscard]] std::uint64_t durablePosition(std::string_view streamId) const {
        const std::scoped_lock lock{publishedMutex};
        const auto             found = published.find(streamId);
        return found != published.end() ? found->second.durablePosition : 0;
    }

    /**
     * @brief The anchor claim the adapter may offer for this instance: its durable position and the chain digest there. Empty before any confirmation.
     *
     * Never above what a sync answered durable for, so no anchor ever covers a record the storage has not confirmed (7.3, 9.3).
     */
    [[nodiscard]] std::optional<AnchorClaim> durableClaim(std::string_view streamId) const {
        const std::scoped_lock lock{publishedMutex};
        const auto             found = durableDigests.find(streamId);
        if (found == durableDigests.end() || found->second.first == 0)
            return std::nullopt;
        return makeAnchorClaim(streamId, found->second.first, found->second.second);
    }

private:
    struct Stream {
        explicit Stream(std::string identity) : id(std::move(identity)), chain(id) {}

        std::string                           id;
        AuditChain                            chain;
        StreamStorageState                    state       = StreamStorageState::Persisting;
        StorageIssue                          cause       = StorageIssue::None;
        bool                                  segmentOpen = false;
        SegmentRef                            segment     = 0;
        std::uint64_t                         segmentEnd  = 0;
        std::uint32_t                         nextIndex   = 0;
        std::uint64_t                         appended    = 0;
        std::uint64_t                         durable     = 0;
        Sha256Digest                          durableDigest{};
        std::size_t                           unsynced = 0;
        std::chrono::steady_clock::time_point oldestUnsynced;
        bool                                  syncUnsupported = false;
        std::vector<std::uint8_t>             lastCanonical;
        /** @brief The ledger is a stream of its own: it may draw on the reserve, and what it loses is not an admitted event (9.6, 10.1). */
        bool isLedger = false;
        /** @brief Highest position the provider accepted from this adapter (7.3). */
        std::uint64_t anchored = 0;
    };

    /** @brief The log as read once, and the recovery computed from it: what a retention attempt decides on (10.3, 10.4, 10.5). */
    struct LogState {
        LogAnalysis        log;
        ChainStateRecovery recovery;
    };

    /** @brief The cached state, read now when there is none. */
    [[nodiscard]] const LogState& current(std::optional<LogState>& cache) {
        if (!cache.has_value()) {
            LogAnalysis        log      = LogAnalysis::read(medium);
            ChainStateRecovery recovery = recoverChainState(log, provider());
            cache.emplace(LogState{.log = std::move(log), .recovery = std::move(recovery)});
        }
        return *cache;
    }

    /** @brief Catches whatever the integrator's provider throws and answers unavailable, as the calls to the medium do. */
    class GuardedProvider final : public AnchorProvider {
    public:
        explicit GuardedProvider(AnchorProvider& wrapped) : inner(&wrapped) {}

        [[nodiscard]] AdvanceAnswer advance(const AnchorClaim& claim) override {
            try {
                return inner->advance(claim);
            } catch (...) {
                return ProviderUnavailable{};
            }
        }
        [[nodiscard]] RetireAnswer retire(std::string_view streamId, std::uint64_t position) override {
            try {
                return inner->retire(streamId, position);
            } catch (...) {
                return ProviderUnavailable{};
            }
        }
        [[nodiscard]] LatestAnswer latest(std::string_view streamId) override {
            try {
                return inner->latest(streamId);
            } catch (...) {
                return ProviderUnavailable{};
            }
        }
        [[nodiscard]] StreamsAnswer streams() override {
            try {
                return inner->streams();
            } catch (...) {
                return ProviderUnavailable{};
            }
        }

    private:
        AnchorProvider* inner;
    };

    struct AtomicCounters {
        std::atomic<std::uint64_t> fullEntries{0};
        std::atomic<std::uint64_t> openFailures{0};
        std::atomic<std::uint64_t> appendFailures{0};
        std::atomic<std::uint64_t> syncFailures{0};
        std::atomic<std::uint64_t> noSpace{0};
        std::atomic<std::uint64_t> duplicateMismatch{0};
        std::atomic<std::uint64_t> sequenceOutOfOrder{0};
        std::atomic<std::uint64_t> notEncodable{0};
        std::atomic<std::uint64_t> mediumUnreadable{0};
        std::atomic<std::uint64_t> syncUnsupported{0};
        std::atomic<std::uint64_t> invalidStream{0};
        std::atomic<std::uint64_t> streamLimitRefused{0};
        std::atomic<std::uint64_t> streamIdentityInUse{0};
        std::atomic<std::uint64_t> notDurableAtFailure{0};
        std::atomic<std::uint64_t> recoveryFindings{0};
        std::atomic<std::uint64_t> reservedActionRefused{0};
        std::atomic<std::uint64_t> ledgerRefused{0};
        std::atomic<std::uint64_t> ledgerRecords{0};
        std::atomic<std::uint64_t> trimsRecorded{0};
        std::atomic<std::uint64_t> segmentsReclaimed{0};
        std::atomic<std::uint64_t> streamsRemoved{0};
        std::atomic<std::uint64_t> retirements{0};
        std::atomic<std::uint64_t> retentionRefused{0};
        std::atomic<std::uint64_t> anchorsAccepted{0};
        std::atomic<std::uint64_t> anchorsRefused{0};
        std::atomic<std::uint64_t> anchorsUnavailable{0};
        std::atomic<std::uint64_t> integrityFaults{0};
    };

    PersistingAuditSink(StorageMedium& storage, StorageConfig declared, std::size_t reserved, RecoveryReport recovered)
        : medium(storage), config(std::move(declared)), reserve(reserved), startup(std::move(recovered)) {
        if (config.provider != nullptr)
            guarded = std::make_unique<GuardedProvider>(*config.provider);
        std::ranges::sort(startup.streamsHeld);
        counters.recoveryFindings.store(startup.findings.size(), std::memory_order_relaxed);
        (void)refreshFree();
    }

    [[nodiscard]] StorageCounters counterSnapshot() const {
        return {.fullEntries           = counters.fullEntries.load(std::memory_order_relaxed),
                .openFailures          = counters.openFailures.load(std::memory_order_relaxed),
                .appendFailures        = counters.appendFailures.load(std::memory_order_relaxed),
                .syncFailures          = counters.syncFailures.load(std::memory_order_relaxed),
                .noSpace               = counters.noSpace.load(std::memory_order_relaxed),
                .duplicateMismatch     = counters.duplicateMismatch.load(std::memory_order_relaxed),
                .sequenceOutOfOrder    = counters.sequenceOutOfOrder.load(std::memory_order_relaxed),
                .notEncodable          = counters.notEncodable.load(std::memory_order_relaxed),
                .mediumUnreadable      = counters.mediumUnreadable.load(std::memory_order_relaxed),
                .syncUnsupported       = counters.syncUnsupported.load(std::memory_order_relaxed),
                .invalidStream         = counters.invalidStream.load(std::memory_order_relaxed),
                .streamLimitRefused    = counters.streamLimitRefused.load(std::memory_order_relaxed),
                .streamIdentityInUse   = counters.streamIdentityInUse.load(std::memory_order_relaxed),
                .notDurableAtFailure   = counters.notDurableAtFailure.load(std::memory_order_relaxed),
                .recoveryFindings      = counters.recoveryFindings.load(std::memory_order_relaxed),
                .reservedActionRefused = counters.reservedActionRefused.load(std::memory_order_relaxed),
                .ledgerRefused         = counters.ledgerRefused.load(std::memory_order_relaxed),
                .ledgerRecords         = counters.ledgerRecords.load(std::memory_order_relaxed),
                .trimsRecorded         = counters.trimsRecorded.load(std::memory_order_relaxed),
                .segmentsReclaimed     = counters.segmentsReclaimed.load(std::memory_order_relaxed),
                .streamsRemoved        = counters.streamsRemoved.load(std::memory_order_relaxed),
                .retirements           = counters.retirements.load(std::memory_order_relaxed),
                .retentionRefused      = counters.retentionRefused.load(std::memory_order_relaxed),
                .anchorsAccepted       = counters.anchorsAccepted.load(std::memory_order_relaxed),
                .anchorsRefused        = counters.anchorsRefused.load(std::memory_order_relaxed),
                .anchorsUnavailable    = counters.anchorsUnavailable.load(std::memory_order_relaxed),
                .integrityFaults       = counters.integrityFaults.load(std::memory_order_relaxed)};
    }

    void publish(const Stream& stream) {
        const std::scoped_lock lock{publishedMutex};
        auto&                  entry = published[stream.id];
        entry.streamId               = stream.id;
        entry.state                  = stream.state;
        entry.cause                  = stream.cause;
        entry.durablePosition        = stream.durable;
        entry.appendedPosition       = stream.appended;
        entry.segmentsOpened         = stream.nextIndex;
        durableDigests[stream.id]    = {stream.durable, stream.durableDigest};
    }

    void note(StorageIssue issue) noexcept {
        lastIssue.store(issue, std::memory_order_relaxed);
    }

    /** @brief The instance ends: nothing more is written to it, and what was accepted but not confirmed is counted and reported as a loss (9.6). */
    void fail(Stream& stream, StorageIssue issue) {
        if (stream.state == StreamStorageState::Failed)
            return;
        stream.state = StreamStorageState::Failed;
        stream.cause = issue;
        note(issue);
        // What the ledger had written but not confirmed was never an event a producer was admitted for.
        const std::uint64_t lost = !stream.isLedger && stream.appended > stream.durable ? stream.appended - stream.durable : 0;
        counters.notDurableAtFailure.fetch_add(lost, std::memory_order_relaxed);
        publish(stream);
        if (lost != 0 && config.reportLoss) {
            try {
                config.reportLoss(lost);
            } catch (...) {  // NOLINT(bugprone-empty-catch): the loss is already in the counters; a failing reporter must not mask the failure.
            }
        }
    }

    void enterFull(Stream& stream) {
        if (stream.state == StreamStorageState::Full)
            return;
        stream.state = StreamStorageState::Full;
        stream.cause = StorageIssue::Full;
        counters.fullEntries.fetch_add(1, std::memory_order_relaxed);
        note(StorageIssue::Full);
        publish(stream);
    }

    /** @brief Free segments beyond the reserve, counted from the medium; empty when it cannot say. */
    [[nodiscard]] std::optional<std::uint64_t> refreshFree() {
        std::optional<std::vector<SegmentInfo>> listing;
        try {
            listing = medium.segments();
        } catch (...) {
            listing.reset();
        }
        if (!listing)
            return std::nullopt;
        const std::uint64_t used   = listing->size();
        const std::uint64_t free   = used < config.segmentCount ? config.segmentCount - used : 0;
        const std::uint64_t beyond = free > reserve ? free - reserve : 0;
        freeTotal.store(free, std::memory_order_relaxed);
        freeBeyondReserve.store(beyond, std::memory_order_relaxed);
        return beyond;
    }

    [[nodiscard]] OpenAnswer callOpen(const SegmentOpening& opening) noexcept {
        try {
            return medium.open(opening);
        } catch (...) {
            return {};
        }
    }
    [[nodiscard]] AppendAnswer callAppend(SegmentRef segment, std::span<const std::uint8_t> bytes) noexcept {
        try {
            return medium.append(segment, bytes);
        } catch (...) {
            return {};
        }
    }
    [[nodiscard]] SyncAnswer callSync(SegmentRef segment, std::uint64_t offset) noexcept {
        try {
            return medium.sync(segment, offset);
        } catch (...) {
            return SyncAnswer::Failed;
        }
    }

    /**
     * @brief Confirm everything appended to this stream: a sync of the open segment up to its end (9.3). False only when the instance failed.
     *
     * Earlier segments were synced before the rotation that left them, so a Durable answer here covers the whole prefix, and the durable position
     * becomes the last appended sequence, never above it. Unsupported is remembered and is not a failure.
     */
    [[nodiscard]] bool sync(Stream& stream) {
        if (stream.syncUnsupported || !stream.segmentOpen || stream.appended == stream.durable) {
            stream.unsynced = 0;
            return true;
        }
        switch (callSync(stream.segment, stream.segmentEnd)) {
            case SyncAnswer::Durable:
                stream.durable       = stream.appended;
                stream.durableDigest = stream.chain.headDigest();
                stream.unsynced      = 0;
                publish(stream);
                return true;
            case SyncAnswer::Unsupported:
                stream.syncUnsupported = true;
                stream.unsynced        = 0;
                counters.syncUnsupported.fetch_add(1, std::memory_order_relaxed);
                return true;
            case SyncAnswer::Failed:
                break;
        }
        counters.syncFailures.fetch_add(1, std::memory_order_relaxed);
        fail(stream, StorageIssue::SyncFailed);
        return false;
    }

    /** @brief Open the next segment for `firstSequence`, syncing the one it follows. False when the event must wait (Full) or the instance failed. */
    [[nodiscard]] bool rotate(Stream& stream, std::uint64_t firstSequence) {
        const auto beyond = refreshFree();
        if (!beyond) {
            counters.mediumUnreadable.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::MediumUnreadable);
            return false;
        }
        // The ledger opens ordinary free segments while there are any and draws on the reserve only when there are none (9.6).
        const std::uint64_t room = stream.isLedger ? freeTotal.load(std::memory_order_relaxed) : *beyond;
        if (room == 0) {
            if (stream.isLedger) {
                counters.noSpace.fetch_add(1, std::memory_order_relaxed);
                fail(stream, StorageIssue::NoSpace);
            } else {
                enterFull(stream);
            }
            return false;
        }
        if (stream.segmentOpen && !sync(stream))
            return false;
        const auto opening = encodeSegmentOpening(stream.id, stream.nextIndex, firstSequence);
        const auto answer  = callOpen({.streamId = stream.id, .segmentIndex = stream.nextIndex, .firstSequence = firstSequence, .bytes = opening});
        if (answer.status == OpenStatus::NoSpace) {
            // The adapter never opens beyond the free segments it counts, so the medium's capacity was declared wrongly (9.6).
            counters.noSpace.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::NoSpace);
            return false;
        }
        if (answer.status != OpenStatus::Opened || answer.end != opening.size()) {
            counters.openFailures.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::OpenFailed);
            return false;
        }
        stream.segmentOpen = true;
        stream.segment     = answer.segment;
        stream.segmentEnd  = answer.end;
        ++stream.nextIndex;
        stream.state = StreamStorageState::Persisting;
        stream.cause = StorageIssue::None;
        (void)refreshFree();
        publish(stream);
        return true;
    }

    [[nodiscard]] bool append(Stream& stream, const core::AuditEvent& event) {
        const auto canonical = CanonicalRecord::encode(event);
        if (!canonical) {
            counters.notEncodable.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::NotEncodable);
            return false;
        }
        const std::uint64_t sequence = event.sequence();
        if (stream.appended != 0 && sequence == stream.appended) {
            // A retry after an uncertain failure (ADR-002 Decision 3): identical bytes are accepted without a second append, other bytes are an integrity
            // fault.
            if (std::ranges::equal(canonical->bytes(), stream.lastCanonical))
                return true;
            counters.duplicateMismatch.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::DuplicateMismatch);
            return false;
        }
        if (sequence != stream.appended + 1) {
            counters.sequenceOutOfOrder.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::SequenceOutOfOrder);
            return false;
        }
        const std::size_t frameSize = recordFrameSize(canonical->size());
        if (!stream.segmentOpen || stream.segmentEnd + frameSize > config.segmentSize) {
            if (!rotate(stream, sequence))
                return false;
        }
        const auto chained = stream.chain.append(event);
        if (!chained) {
            counters.sequenceOutOfOrder.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::SequenceOutOfOrder);
            return false;
        }
        const auto frame  = encodeRecordFrame(chained->canonical.bytes(), chained->digest);
        const auto answer = callAppend(stream.segment, frame);
        if (answer.status != AppendStatus::Written || answer.end != stream.segmentEnd + frame.size()) {
            // Never retried at the same offset, which could overwrite part of a frame (9.6).
            counters.appendFailures.fetch_add(1, std::memory_order_relaxed);
            fail(stream, StorageIssue::AppendFailed);
            return false;
        }
        const auto now    = config.clock();
        stream.segmentEnd = answer.end;
        stream.appended   = sequence;
        stream.lastCanonical.assign(canonical->bytes().begin(), canonical->bytes().end());
        if (stream.unsynced++ == 0)
            stream.oldestUnsynced = now;
        publish(stream);
        if (stream.unsynced >= config.sync.recordBound || (config.sync.ageBound && now - stream.oldestUnsynced >= *config.sync.ageBound))
            (void)sync(stream);  // a failure here ends the instance but the event was already handed off: it is counted as not durable
        return true;
    }

    [[nodiscard]] AnchorProvider* provider() noexcept {
        return guarded.get();
    }

    /** @brief Report suspect streams and ledgers through the health signal, never as an audit event and never to the medium (9.6, 10.3). Each is reported once.
     */
    void recordFaults(std::span<const IntegrityFault> faults) {
        std::size_t added = 0;
        {
            const std::scoped_lock lock{publishedMutex};
            for (const IntegrityFault& found : faults) {
                const auto same = [&](const IntegrityFault& known) {
                    return known.kind == found.kind && known.stream == found.stream && known.position == found.position;
                };
                if (std::ranges::none_of(integrityFaults, same)) {
                    integrityFaults.push_back(found);
                    ++added;
                }
            }
        }
        if (added != 0) {
            counters.integrityFaults.fetch_add(added, std::memory_order_relaxed);
            note(StorageIssue::IntegrityFault);
        }
    }

    [[nodiscard]] core::RawTime ledgerTime() {
        if (!config.ledger.has_value() || !config.ledger->time)
            return core::RawTime::unavailable();
        try {
            return config.ledger->time();
        } catch (...) {
            return core::RawTime::unavailable();
        }
    }

    /**
     * @brief Append one ledger record and confirm it before anything else is written (10.2). False when the ledger failed.
     *
     * On a medium that answers Unsupported the record is stored but never confirmed, so the ordering of 10.2 cannot hold, and nothing is ever removed on the
     * strength of it (see retain()).
     */
    [[nodiscard]] bool writeLedger(const LedgerEntry& entry) {
        if (ledger == nullptr || ledger->state == StreamStorageState::Failed)
            return false;
        const auto event = buildLedgerEvent(entry, ledger->id, ledger->appended + 1, ledgerTime());
        if (!event.has_value()) {
            counters.notEncodable.fetch_add(1, std::memory_order_relaxed);
            fail(*ledger, StorageIssue::NotEncodable);
            return false;
        }
        if (!append(*ledger, *event))
            return false;
        counters.ledgerRecords.fetch_add(1, std::memory_order_relaxed);
        return sync(*ledger);
    }

    /** @brief The ledger's last record is durably confirmed: only a medium that answered Durable can say so (9.3). */
    [[nodiscard]] bool ledgerConfirmed() const noexcept {
        return ledger != nullptr && ledger->state != StreamStorageState::Failed && ledger->appended != 0 && ledger->durable == ledger->appended;
    }

    /**
     * @brief Recover chain state, then write record 1 and the `recovered` records (10.2, 10.3), then complete the retirements an earlier start left owed (7.3).
     *
     * Nothing is repaired, rewritten or reordered: an inconsistent state is written as the `Failed` form, citing the last position that checks, and reported
     * through health. A ledger that cannot write its start is failed, and no producer event is stored.
     */
    void startLedger() {
        ledger                    = std::make_unique<Stream>(config.ledger.value_or(LedgerConfig{}).streamId);
        ledger->isLedger          = true;
        restartInfo.ledgerEnabled = true;
        publish(*ledger);

        const LogAnalysis        log      = LogAnalysis::read(medium);
        const ChainStateRecovery recovery = recoverChainState(log, provider());
        rememberNamedIdentities(log);
        restartInfo.faults = recovery.faults;
        restartInfo.fork   = recovery.fork;
        recordFaults(recovery.faults);

        LedgerEntry first = LedgerEntry::origin(ledger->id);
        if (recovery.predecessor.has_value()) {
            const auto earlier              = recovery.held.find(*recovery.predecessor);
            const bool checks               = earlier != recovery.held.end() && earlier->second.consistent;
            first                           = LedgerEntry::predecessor(*recovery.predecessor,
                                             checks,
                                             earlier != recovery.held.end() ? earlier->second.position : std::nullopt,
                                             earlier != recovery.held.end() ? earlier->second.digest : std::nullopt);
            restartInfo.predecessor         = *recovery.predecessor;
            restartInfo.predecessorChecks   = checks;
            restartInfo.predecessorPosition = first.sourceSequence;
        }
        restartInfo.firstRecord = first.kind;
        if (!writeLedger(first))
            return;
        for (const auto& id : recovery.toCite) {
            const auto found = recovery.held.find(id);
            if (found == recovery.held.end())
                continue;
            const RecoveredStream& state = found->second;
            if (!writeLedger(LedgerEntry::recovered(id, state.consistent, state.position, state.digest)))
                return;
            ++(state.consistent ? restartInfo.recoveredChecked : restartInfo.recoveredFailed);
        }
        restartInfo.startWritten = true;
        if (guarded == nullptr)
            return;
        for (const PendingRetirement& owed : recovery.pendingRetirements) {
            if (std::holds_alternative<AnchorStamp>(guarded->retire(owed.stream, owed.position))) {
                counters.retirements.fetch_add(1, std::memory_order_relaxed);
                ++restartInfo.retirementsCompleted;
            }
        }
    }

    /** @brief Every identity a ledger in the log names, as a ledger, an opened, closed, trimmed or recovered stream: none may start a new instance (10.3). */
    void rememberNamedIdentities(const LogAnalysis& log) {
        std::set<std::string, std::less<>> names;
        for (const LedgerImage& ledgerImage : log.ledgers()) {
            names.insert(ledgerImage.id);
            names.insert(ledgerImage.opened.begin(), ledgerImage.opened.end());
            for (const auto& [stream, close] : ledgerImage.closes)
                names.insert(stream);
            for (const TrimRecord& trim : ledgerImage.trims)
                names.insert(trim.stream);
            for (const Citation& citation : ledgerImage.recovered)
                names.insert(citation.target);
        }
        namedByLedgers.assign(names.begin(), names.end());
    }

    [[nodiscard]] bool closeOne(Stream& stream) {
        if (stream.state == StreamStorageState::Closed)
            return true;
        if (stream.state == StreamStorageState::Failed || !sync(stream))
            return false;
        (void)advanceAnchor(stream);
        if (ledger != nullptr && !writeLedger(LedgerEntry::streamClose(stream.id, stream.appended, stream.chain.headDigest())))
            return false;
        stream.state = StreamStorageState::Closed;
        stream.cause = StorageIssue::None;
        publish(stream);
        return true;
    }

    [[nodiscard]] bool advanceAnchor(Stream& stream) {
        if (guarded == nullptr || stream.durable == 0 || stream.durable <= stream.anchored)
            return false;
        const AdvanceAnswer answer = guarded->advance(makeAnchorClaim(stream.id, stream.durable, stream.durableDigest));
        if (std::holds_alternative<AnchorStamp>(answer)) {
            stream.anchored = stream.durable;
            counters.anchorsAccepted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (std::holds_alternative<AdvanceRefusal>(answer)) {
            // A fresh claim the provider refuses means the log or the provider has diverged (7.3): never a transient failure.
            counters.anchorsRefused.fetch_add(1, std::memory_order_relaxed);
            const IntegrityFault diverged{.kind = IntegrityFaultKind::AnchorDiverged, .stream = stream.id, .position = stream.durable};
            recordFaults(std::span{&diverged, 1});
            return false;
        }
        counters.anchorsUnavailable.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /**
     * @brief The ledger holds the highest trim of a stream that still needs it: one the log still holds, whose prefix that trim accounts for, or a removed one
     * whose anchor the provider has not replaced by a retirement, whose owed retirement that trim authorizes. Without a provider answer the trim is kept.
     */
    [[nodiscard]] bool holdsNeededTrim(const LogAnalysis& log, const LedgerImage& old) {
        for (const TrimRecord& trim : old.trims) {
            const bool coveredElsewhere = std::ranges::any_of(log.ledgers(), [&](const LedgerImage& other) {
                return other.id != old.id && std::ranges::any_of(other.trims, [&](const TrimRecord& recorded) {
                           return recorded.stream == trim.stream && recorded.position >= trim.position;
                       });
            });
            if (coveredElsewhere)
                continue;
            if (log.image().find(trim.stream) != nullptr)
                return true;
            if (guarded != nullptr) {
                const LatestAnswer answer = guarded->latest(trim.stream);
                if (std::holds_alternative<Anchor>(answer) || std::holds_alternative<ProviderUnavailable>(answer))
                    return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool callReclaim(SegmentRef segment) noexcept {
        try {
            return medium.reclaim(segment);
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief Rotation (`whole` false) or whole-stream retention (`whole` true), 10.4 and 10.5.
     *
     * The order keeps an interruption on the safe side: the trim record is durably confirmed first, then the segments go, then the provider is told. A medium
     * that cannot confirm the trim never loses a segment on its account.
     */
    [[nodiscard]] RetentionResult retain(std::string_view target, bool whole, std::optional<LogState>* cache) {
        const auto refuse = [&](RetentionOutcome outcome, std::uint64_t recordedTrim = 0) {
            counters.retentionRefused.fetch_add(1, std::memory_order_relaxed);
            return RetentionResult{.outcome = outcome, .trimmedThrough = recordedTrim};
        };
        if (ledger == nullptr || ledger->state == StreamStorageState::Failed || closed)
            return refuse(RetentionOutcome::NoLedger);
        if (ledger->id == target)
            return refuse(RetentionOutcome::NotRotatable);
        // A trim the medium can never confirm would be written again at each attempt, and a ledger's trims must increase (10.2): write none at all.
        if (ledger->syncUnsupported)
            return refuse(RetentionOutcome::NotConfirmed);

        std::optional<LogState>   local;
        const LogState&           read     = current(cache != nullptr ? *cache : local);
        const LogAnalysis&        log      = read.log;
        const ChainStateRecovery& recovery = read.recovery;
        const StreamEvaluation*   ev       = log.evaluation(target);
        const StreamImage*        image    = log.image().find(target);
        if (log.image().unreadable())
            return refuse(RetentionOutcome::MediumUnreadable);
        if (ev == nullptr || !ev->inImage || image == nullptr)
            return refuse(RetentionOutcome::UnknownStream);
        const bool ledgerTarget = log.isLedger(target);
        if (ledgerTarget && !whole)
            return refuse(RetentionOutcome::NotRotatable);
        const auto live = streams.find(target);
        if (live != streams.end()) {
            const StreamStorageState state = live->second.state;
            if (state == StreamStorageState::Failed || (whole && state != StreamStorageState::Closed))
                return refuse(RetentionOutcome::StreamNotEnded);
        }

        // Retention never removes records of a stream found inconsistent (10.3).
        if (const auto held = recovery.held.find(target); held == recovery.held.end() || !held->second.consistent) {
            for (const IntegrityFault& found : recovery.faults) {
                if (found.stream == target)
                    recordFaults(std::span{&found, 1});
            }
            return refuse(RetentionOutcome::StreamInconsistent);
        }
        if (ledgerTarget) {
            const LedgerImage* old         = log.ledger(target);
            const bool         streamsGone = std::ranges::none_of(old->opened, [&](const std::string& opened) {
                return log.image().find(opened) != nullptr;
            });
            const bool         cited       = std::ranges::any_of(log.ledgers(), [&](const LedgerImage& other) {
                return other.id != target && other.predecessor.has_value() && other.predecessor->target == target;
            });
            if (!streamsGone || !cited || holdsNeededTrim(log, *old))
                return refuse(RetentionOutcome::LedgerStillNeeded);
        }

        const auto&   segments       = image->segments;
        std::uint64_t q              = 0;
        bool          retire         = false;
        std::uint64_t retirePosition = 0;
        std::uint64_t bound          = std::numeric_limits<std::uint64_t>::max();
        if (guarded != nullptr) {
            const LatestAnswer answer = guarded->latest(target);
            if (const auto* anchor = std::get_if<Anchor>(&answer)) {
                bound          = anchor->position;
                retire         = whole;
                retirePosition = anchor->position;
            } else if (std::holds_alternative<Retirement>(answer)) {
                return refuse(RetentionOutcome::StreamInconsistent);
            } else if (std::holds_alternative<AnchorAbsent>(answer)) {
                if (!whole)
                    return refuse(RetentionOutcome::NoAnchor);
            } else if (!whole && live != streams.end() && live->second.anchored != 0) {
                // The provider did not answer: only an anchor this adapter itself holds from its own advance may be relied on (10.4).
                bound = live->second.anchored;
            } else {
                return refuse(RetentionOutcome::ProviderUnavailable);
            }
        }
        if (whole) {
            q = ev->checkedThrough;
        } else {
            // The last record of the latest whole segment that ends at or before the bound; never the last segment, which is the open one for a live stream.
            for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
                if (segments[i].recordCount == 0 || segments[i].lastSequence() > bound)
                    break;
                q = segments[i].lastSequence();
            }
            if (q == 0)
                return refuse(RetentionOutcome::NothingToTrim);
        }
        if (whole && q == 0 && ev->recordCount != 0)
            return refuse(RetentionOutcome::StreamInconsistent);

        RetentionResult result{.outcome = whole ? RetentionOutcome::Removed : RetentionOutcome::Trimmed, .trimmedThrough = q};
        std::uint64_t   trimSequence = 0;
        if (q != 0) {
            const auto digest = ev->recomputedDigestAt(q);
            if (!digest.has_value())
                return refuse(RetentionOutcome::StreamInconsistent);
            if (const auto done = sessionTrims.find(target); done != sessionTrims.end() && done->second.first >= q) {
                trimSequence = done->second.second;  // already recorded and confirmed this start: the ledger's trims stay increasing (10.2)
            } else {
                if (!writeLedger(LedgerEntry::streamTrim(target, q, *digest)))
                    return refuse(RetentionOutcome::NotConfirmed);
                if (!ledgerConfirmed())
                    return refuse(RetentionOutcome::NotConfirmed, q);
                trimSequence                      = ledger->appended;
                sessionTrims[std::string{target}] = {q, trimSequence};
                counters.trimsRecorded.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (retire) {
            // An interruption between the removal and the retirement leaves a retirement owed. The next start may relay it only if an anchor of the ledger
            // covers the trim, so the ledger is anchored past it before anything is removed (7.2, condition 5).
            (void)advanceAnchor(*ledger);
            if (ledger->anchored < trimSequence)
                return refuse(RetentionOutcome::LedgerNotAnchored, q);
        }

        for (const SegmentImage& segment : segments) {
            if (segment.recordCount != 0 && segment.lastSequence() > q)
                break;
            if (!callReclaim(segment.ref)) {
                result.outcome = RetentionOutcome::ReclaimInterrupted;
                break;
            }
            ++result.segmentsReclaimed;
        }
        counters.segmentsReclaimed.fetch_add(result.segmentsReclaimed, std::memory_order_relaxed);
        (void)refreshFree();
        if (result.outcome == RetentionOutcome::Removed) {
            counters.streamsRemoved.fetch_add(1, std::memory_order_relaxed);
            if (retire && std::holds_alternative<AnchorStamp>(guarded->retire(target, retirePosition))) {
                counters.retirements.fetch_add(1, std::memory_order_relaxed);
                result.retired = true;
            }
        }
        return result;
    }

    StorageMedium& medium;
    StorageConfig  config;
    std::size_t    reserve;
    RecoveryReport startup;
    bool           closed = false;

    std::map<std::string, Stream, std::less<>> streams;
    AtomicCounters                             counters;
    std::atomic<StorageIssue>                  lastIssue{StorageIssue::None};
    std::atomic<std::uint64_t>                 freeBeyondReserve{0};
    std::atomic<std::uint64_t>                 freeTotal{0};

    std::unique_ptr<GuardedProvider> guarded;
    std::unique_ptr<Stream>          ledger;
    RestartReport                    restartInfo;
    /** @brief The trims this start recorded and confirmed: the position, and where the ledger recorded it. Keeps a ledger's trims increasing (10.2). */
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>, std::less<>> sessionTrims;
    /** @brief Identities the ledgers in the log named at start, sorted: none may start a new instance. */
    std::vector<std::string> namedByLedgers;

    mutable std::mutex                                                         publishedMutex;
    std::map<std::string, StreamStorageHealth, std::less<>>                    published;
    std::map<std::string, std::pair<std::uint64_t, Sha256Digest>, std::less<>> durableDigests;
    std::vector<IntegrityFault>                                                integrityFaults;
};

}  // namespace mddlog::adapter
