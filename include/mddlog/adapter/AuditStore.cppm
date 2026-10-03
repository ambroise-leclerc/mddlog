/** @brief Persisting audit sink: segments, durable confirmation and storage health (ADR-004 Decisions 9.3, 9.5 and 9.6). Adapter zone only. */

export module mddlog.adapter.auditstore;

import std;
export import mddlog.sinks.auditsink;
export import mddlog.adapter.auditchain;
export import mddlog.adapter.auditlayout;
export import mddlog.adapter.auditmedium;
export import mddlog.adapter.auditanchor;
export import mddlog.adapter.auditverifier;

export namespace mddlog::adapter {

/** @brief Where a stream instance stands on the medium (9.6). */
enum class StreamStorageState : std::uint8_t {
    /** @brief Appending, and confirming per the sync policy. */
    Persisting,
    /** @brief It needs a segment and only the ledger's reserve is free. Nothing is appended, nothing is skipped; events stay in the ring (9.6). */
    Full,
    /** @brief The instance ended on a storage failure or an integrity fault. Nothing more is written to it (9.6). */
    Failed
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
    MediumUnreadable
};

/** @brief When a sync is issued, besides the one before a rotation and the one on orderly close (9.3). */
struct SyncPolicy {
    /** @brief Sync once this many appended records are unconfirmed; 1 is a sync per record. Zero is invalid. */
    std::size_t recordBound = 1;
    /** @brief Also sync once the oldest unconfirmed record is this old. Checked at each accept() and each tick(). Must be positive. */
    std::optional<std::chrono::nanoseconds> ageBound;
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
};

enum class StorageConfigError : std::uint8_t {
    /** @brief A segment cannot hold its opening and the largest record frame. */
    SegmentTooSmall,
    NoProducerStreams,
    InvalidSyncPolicy,
    /** @brief The medium cannot hold the ledger's reserve and one more segment (9.6). */
    MediumTooSmall
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

/** @brief The records of one stream instance as a reader holds them, ready for AnchorVerifier. Owns the bytes the records point into. */
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
    /** @brief Indices of the segments that held a valid header for this instance, in order. */
    [[nodiscard]] const std::vector<std::uint32_t>& segmentIndices() const noexcept {
        return indices;
    }
    /** @brief Bytes after the last valid frame of the last segment: the trace of a cut, never parsed. */
    [[nodiscard]] std::size_t trailingBytesOfLastSegment() const noexcept {
        return trailing;
    }
    /** @brief False when the medium could not be listed or a segment of the instance could not be read. */
    [[nodiscard]] bool complete() const noexcept {
        return readable;
    }

private:
    friend StoredStream readStoredStream(StorageMedium&, std::string_view);

    std::vector<std::vector<std::uint8_t>> buffers;
    std::vector<StoredRecord>              stored;
    std::vector<std::uint32_t>             indices;
    std::size_t                            trailing = 0;
    bool                                   readable = true;
};

/** @brief Read back the records of `streamId` from every segment whose header names it. Records are not verified: that is AnchorVerifier's job. */
[[nodiscard]] inline StoredStream readStoredStream(StorageMedium& medium, std::string_view streamId) {
    StoredStream out;
    const auto   listing = medium.segments();
    if (!listing) {
        out.readable = false;
        return out;
    }
    struct Found {
        std::uint32_t index  = 0;
        std::size_t   buffer = 0;
    };
    std::vector<Found> found;
    for (const auto& info : *listing) {
        auto bytes = medium.read(info.segment, 0, info.size);
        if (!bytes) {
            out.readable = false;
            continue;
        }
        const SegmentScan scan = scanSegment(*bytes);
        if (scan.status != SegmentStatus::Readable || !scan.header || scan.header->streamId != streamId)
            continue;
        found.push_back({.index = scan.header->segmentIndex, .buffer = out.buffers.size()});
        out.buffers.push_back(std::move(*bytes));
    }
    std::ranges::sort(found, {}, &Found::index);
    for (const auto& item : found) {
        const SegmentScan scan = scanSegment(out.buffers[item.buffer]);
        out.indices.push_back(item.index);
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
    /** @brief Events refused because the medium already holds an instance of that identity: a restart must start a new instance (ADR-002 Decision 5). */
    std::uint64_t streamIdentityInUse = 0;
    /** @brief Events accepted but never confirmed when an instance failed, reported as losses after admission (9.6). */
    std::uint64_t notDurableAtFailure = 0;
    /** @brief Findings of the startup check (9.6). */
    std::uint64_t recoveryFindings = 0;
};

struct StorageHealthSnapshot {
    std::vector<StreamStorageHealth> streams;
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
 * accept(), tick(), flush() and close() belong to the one consumer thread that drains the rings, like AuditSinkAdapter::drainOnce(). health(),
 * durablePosition() and durableClaim() may be called from any thread. The medium must outlive the sink.
 */
class PersistingAuditSink final : public sinks::AuditSink {
public:
    /** @brief Validate the declared configuration against 9.6, run the startup check of the medium, and build the sink. Writes nothing. */
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
        if (!config.clock)
            config.clock = [] {
                return std::chrono::steady_clock::now();
            };
        auto recovered = checkMediumAtStart(medium);
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the constructor is private, so make_shared cannot reach it.
        return std::shared_ptr<PersistingAuditSink>(new PersistingAuditSink(medium, std::move(config), reserve, std::move(recovered)));
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
        const std::string_view id = event.streamId();
        if (!core::AuditEvent::validStreamId(id)) {
            counters.invalidStream.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto it = streams.find(id);
        if (it == streams.end()) {
            if (std::ranges::binary_search(startup.streamsHeld, id)) {
                counters.streamIdentityInUse.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // S bounds the instances open at once: one that failed has ended and no longer counts (9.6).
            std::size_t open = 0;
            for (const auto& entry : streams)
                open += entry.second.state != StreamStorageState::Failed ? 1U : 0U;
            if (open >= config.maxProducerStreams) {
                counters.streamLimitRefused.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            it = streams.try_emplace(std::string{id}, std::string{id}).first;
            publish(it->second);
        }
        Stream& stream = it->second;
        if (stream.state == StreamStorageState::Failed)
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

    /** @brief Sync every stream's open segment now, whatever the policy says. */
    void flush() {
        for (auto& entry : streams) {
            if (entry.second.state != StreamStorageState::Failed)
                (void)sync(entry.second);
        }
    }

    /** @brief Orderly close: a final sync of every stream, then no more events are accepted (9.3). Ledger close records belong to Decision 10. */
    void close() {
        if (closed)
            return;
        flush();
        closed = true;
    }

    [[nodiscard]] StorageHealthSnapshot health() const {
        StorageHealthSnapshot out;
        {
            const std::scoped_lock lock{publishedMutex};
            out.streams.reserve(published.size());
            for (const auto& entry : published)
                out.streams.push_back(entry.second);
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
    };

    PersistingAuditSink(StorageMedium& storage, StorageConfig declared, std::size_t reserved, RecoveryReport recovered)
        : medium(storage), config(std::move(declared)), reserve(reserved), startup(std::move(recovered)) {
        std::ranges::sort(startup.streamsHeld);
        counters.recoveryFindings.store(startup.findings.size(), std::memory_order_relaxed);
        (void)refreshFree();
    }

    [[nodiscard]] StorageCounters counterSnapshot() const {
        return {.fullEntries         = counters.fullEntries.load(std::memory_order_relaxed),
                .openFailures        = counters.openFailures.load(std::memory_order_relaxed),
                .appendFailures      = counters.appendFailures.load(std::memory_order_relaxed),
                .syncFailures        = counters.syncFailures.load(std::memory_order_relaxed),
                .noSpace             = counters.noSpace.load(std::memory_order_relaxed),
                .duplicateMismatch   = counters.duplicateMismatch.load(std::memory_order_relaxed),
                .sequenceOutOfOrder  = counters.sequenceOutOfOrder.load(std::memory_order_relaxed),
                .notEncodable        = counters.notEncodable.load(std::memory_order_relaxed),
                .mediumUnreadable    = counters.mediumUnreadable.load(std::memory_order_relaxed),
                .syncUnsupported     = counters.syncUnsupported.load(std::memory_order_relaxed),
                .invalidStream       = counters.invalidStream.load(std::memory_order_relaxed),
                .streamLimitRefused  = counters.streamLimitRefused.load(std::memory_order_relaxed),
                .streamIdentityInUse = counters.streamIdentityInUse.load(std::memory_order_relaxed),
                .notDurableAtFailure = counters.notDurableAtFailure.load(std::memory_order_relaxed),
                .recoveryFindings    = counters.recoveryFindings.load(std::memory_order_relaxed)};
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
        const std::uint64_t lost = stream.appended > stream.durable ? stream.appended - stream.durable : 0;
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
        if (*beyond == 0) {
            enterFull(stream);
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

    StorageMedium& medium;
    StorageConfig  config;
    std::size_t    reserve;
    RecoveryReport startup;
    bool           closed = false;

    std::map<std::string, Stream, std::less<>> streams;
    AtomicCounters                             counters;
    std::atomic<StorageIssue>                  lastIssue{StorageIssue::None};
    std::atomic<std::uint64_t>                 freeBeyondReserve{0};

    mutable std::mutex                                                         publishedMutex;
    std::map<std::string, StreamStorageHealth, std::less<>>                    published;
    std::map<std::string, std::pair<std::uint64_t, Sha256Digest>, std::less<>> durableDigests;
};

}  // namespace mddlog::adapter
