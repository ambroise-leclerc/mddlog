/** @brief The stored log as one image: its streams, its ledgers, the trims they record, and chain state recovery (ADR-004 10.1, 10.3, 10.4). Adapter zone only.
 */

export module mddlog.adapter.auditlog;

import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditledger;
export import mddlog.adapter.auditverifier;
export import mddlog.adapter.auditlayout;
export import mddlog.adapter.auditmedium;
export import mddlog.adapter.auditanchor;

export namespace mddlog::adapter {

/** @brief One segment of a stream as the medium holds it. */
struct SegmentImage {
    SegmentRef    ref           = 0;
    std::uint32_t index         = 0;
    std::uint64_t firstSequence = 0;
    std::size_t   recordCount   = 0;
    /** @brief Bytes after the last valid frame: unused space, or the trace of a cut. Never parsed. */
    std::size_t trailingBytes = 0;

    /** @brief The sequence of the last record this segment holds; `firstSequence - 1` when it holds none. */
    [[nodiscard]] std::uint64_t lastSequence() const noexcept {
        return firstSequence + recordCount - 1;
    }
};

/** @brief The records of one stream instance, in segment order, and the segments that hold them. The records point into the owning LogImage. */
struct StreamImage {
    std::string               id;
    std::vector<SegmentImage> segments;
    std::vector<StoredRecord> records;
};

/**
 * @brief Everything the medium holds, read once and grouped by the stream each segment's header names (9.4). Owns the bytes the records point into.
 *
 * A segment with no valid preamble or header belongs to no stream: it is counted, and it stops no verdict (9.5). A segment in a layout version this reader
 * does not know cannot be tied to a stream, so it is reported for every stream (9.4).
 */
class LogImage {
public:
    LogImage()                               = default;
    LogImage(const LogImage&)                = delete;
    LogImage& operator=(const LogImage&)     = delete;
    LogImage(LogImage&&) noexcept            = default;
    LogImage& operator=(LogImage&&) noexcept = default;
    ~LogImage()                              = default;

    [[nodiscard]] static LogImage read(StorageMedium& medium) {
        LogImage   out;
        const auto listing = medium.segments();
        if (!listing) {
            out.unreadableFlag = true;
            return out;
        }
        struct Pending {
            SegmentRef    ref = 0;
            SegmentHeader header;
            SegmentScan   scan;
        };
        std::map<std::string, std::vector<Pending>, std::less<>> grouped;
        for (const auto& info : *listing) {
            auto bytes = medium.read(info.segment, 0, info.size);
            if (!bytes) {
                out.unreadableFlag = true;
                continue;
            }
            out.buffers.push_back(std::move(*bytes));
            SegmentScan scan = scanSegment(out.buffers.back());
            switch (scan.status) {
                case SegmentStatus::UnknownLayoutVersion:
                    if (!out.unknownVersion)
                        out.unknownVersion = scan.layoutVersion;
                    break;
                case SegmentStatus::NoValidPreamble:
                case SegmentStatus::NoValidHeader:
                    ++out.headerless;
                    break;
                case SegmentStatus::Readable: {
                    if (!scan.header.has_value())
                        break;
                    SegmentHeader header = *scan.header;
                    const auto    id     = header.streamId;
                    grouped[id].push_back({.ref = info.segment, .header = std::move(header), .scan = std::move(scan)});
                    break;
                }
            }
        }
        for (auto& [id, pending] : grouped) {
            std::ranges::stable_sort(pending, [](const Pending& a, const Pending& b) {
                return a.header.segmentIndex < b.header.segmentIndex;
            });
            StreamImage stream;
            stream.id = id;
            for (const auto& item : pending) {
                stream.segments.push_back({.ref           = item.ref,
                                           .index         = item.header.segmentIndex,
                                           .firstSequence = item.header.firstSequence,
                                           .recordCount   = item.scan.records.size(),
                                           .trailingBytes = item.scan.trailingBytes});
                for (const auto& record : item.scan.records)
                    stream.records.push_back({.bytes = record.canonical, .digest = record.digest});
            }
            out.held.emplace(id, std::move(stream));
        }
        return out;
    }

    [[nodiscard]] const std::map<std::string, StreamImage, std::less<>>& streams() const noexcept {
        return held;
    }
    [[nodiscard]] const StreamImage* find(std::string_view id) const {
        const auto found = held.find(id);
        return found == held.end() ? nullptr : &found->second;
    }
    /** @brief The medium could not be listed, or a segment could not be read: records may be missing for that reason alone. */
    [[nodiscard]] bool unreadable() const noexcept {
        return unreadableFlag;
    }
    [[nodiscard]] std::optional<std::uint16_t> unknownLayoutVersion() const noexcept {
        return unknownVersion;
    }
    /** @brief Segments with no valid preamble or header: the usual trace of a cut during `open` (9.5). */
    [[nodiscard]] std::size_t segmentsWithoutHeader() const noexcept {
        return headerless;
    }

    /** @brief What the verifier needs of a stream's segments, from segment `skip` on. */
    [[nodiscard]] StoredLayout layoutOf(const StreamImage& stream, std::size_t skip = 0) const {
        StoredLayout out;
        out.unreadable           = unreadableFlag;
        out.unknownLayoutVersion = unknownVersion;
        for (std::size_t i = skip; i < stream.segments.size(); ++i)
            out.segments.push_back(
                {.segmentIndex = stream.segments[i].index, .firstSequence = stream.segments[i].firstSequence, .recordCount = stream.segments[i].recordCount});
        return out;
    }

private:
    std::vector<std::vector<std::uint8_t>>          buffers;
    std::map<std::string, StreamImage, std::less<>> held;
    bool                                            unreadableFlag = false;
    std::optional<std::uint16_t>                    unknownVersion;
    std::size_t                                     headerless = 0;
};

/** @brief A recorded trim: records `1 … position` of `stream` were about to be removed, and `digest` is H at `position` (10.2, 10.4). */
struct TrimRecord {
    std::string   stream;
    std::uint64_t position = 0;
    Sha256Digest  digest{};
    /** @brief The ledger that recorded it, and its position there. */
    std::string   ledger;
    std::uint64_t ledgerSequence = 0;
};

/** @brief A `predecessor` or `recovered` record: what the adapter recorded of an earlier ledger or stream at a restart (10.2). */
struct Citation {
    std::string target;
    /** @brief The `Executed` form: the target was recovered and checks. `Failed` cites the last position that does, or nothing. */
    bool                         checks = false;
    std::optional<std::uint64_t> position;
    std::optional<Sha256Digest>  digest;
    std::uint64_t                ledgerSequence = 0;
};

struct CloseRecord {
    std::uint64_t position = 0;
    Sha256Digest  digest{};
    std::uint64_t ledgerSequence = 0;
};

struct LedgerFaultAt {
    std::uint64_t sequence = 0;
    LedgerFault   fault    = LedgerFault::UnknownReservedAction;
};

/**
 * @brief One ledger as the log holds it (10.1, 10.2). Only the records up to the last one whose chain link checks are read: what a broken chain says after
 * the break is not evidence of anything.
 */
struct LedgerImage {
    std::string                id;
    LedgerRecordKind           head            = LedgerRecordKind::Origin;
    std::uint64_t              recordsPresent  = 0;
    std::uint64_t              verifiedThrough = 0;
    Sha256Digest               headDigest{};
    bool                       chainBroken = false;
    std::vector<LedgerFaultAt> faults;
    std::optional<Citation>    predecessor;
    std::vector<Citation>      recovered;
    /** @brief Streams the ledger opened, in order. */
    std::vector<std::string>           opened;
    std::map<std::string, CloseRecord> closes;
    std::vector<TrimRecord>            trims;
    /** @brief `mddlog.ledger.close` is present and valid. */
    bool closedOrderly = false;

    /** @brief The ledger's records are all valid and its chain checks through its last record present. */
    [[nodiscard]] bool wellFormed() const noexcept {
        return !chainBroken && faults.empty();
    }
};

/** @brief How the records of a stream stand against what the ledgers recorded about its prefix (10.4, 10.6). */
enum class StreamDisposition : std::uint8_t {
    /** @brief Records start at 1 and no trim is recorded. */
    Whole,
    /** @brief Records start at `q + 1` after a complete removal. */
    Rotated,
    /** @brief A trim is recorded and every record is still present: the removal did not happen. */
    RemovalNotCarriedOut,
    /** @brief Records `k … q` remain with `k` above 1: an interrupted removal left them. */
    InterruptedRemoval,
    /** @brief Records `q + 1 … k - 1` are missing between the trim and the first record present. */
    GapAfterTrim,
    /** @brief Records `1 … k - 1` are missing and no trim accounts for them. */
    PrefixMissing,
    /** @brief The trim's position falls inside a segment. */
    TrimNotOnSegmentBoundary,
    /** @brief No segment is present and a trim is recorded. */
    Removed,
    /** @brief No segment is present and no trim is recorded. */
    Absent
};

/** @brief Where one stream's chain was walked from, what it reached, and what the walk says about its prefix. */
struct StreamEvaluation {
    std::string       id;
    StreamDisposition disposition = StreamDisposition::Absent;
    /** @brief Any segment of the stream is on the medium. */
    bool          inImage      = false;
    std::uint64_t firstPresent = 0;
    std::size_t   recordCount  = 0;
    std::size_t   segmentCount = 0;
    /** @brief Every valid trim recorded for the stream, by position; `trim` is the highest. */
    std::vector<TrimRecord>   trims;
    std::optional<TrimRecord> trim;

    /** @brief What the anchor verifier is given: its start, and how many leading records and segments it is not given (leftover of an interrupted removal). */
    StreamStart start;
    std::size_t skipRecords  = 0;
    std::size_t skipSegments = 0;

    /** @brief The chain walk over the records the verifier is given (or, for a missing prefix, over those after the first). */
    std::uint64_t                walkStartPosition = 0;
    Sha256Digest                 walkStartDigest   = chainInitialValue;
    std::vector<Sha256Digest>    walkDigests;
    std::uint64_t                checkedThrough = 0;
    Sha256Digest                 checkedDigest  = chainInitialValue;
    std::optional<std::uint64_t> failedAt;
    ChainFinding                 finding      = ChainFinding::Ok;
    bool                         cannotVerify = false;

    /** @brief The leftover records of an interrupted removal, walked from the first record's stored digest. */
    std::uint64_t             leftoverStartPosition = 0;
    Sha256Digest              leftoverStartDigest   = chainInitialValue;
    std::vector<Sha256Digest> leftoverDigests;
    /** @brief The leftover reaches the trim's digest at the trim's position. */
    bool leftoverChecks = true;
    /** @brief Every recorded trim whose position the records reach carries the digest they reproduce. */
    bool trimDigestOk = true;
    /** @brief A trim cites a position past the last record present while the prefix is still there. */
    bool trimExceedsRecords = false;
    /** @brief The expected sequence of the first segment header that does not follow (9.5). */
    std::optional<std::uint64_t> layoutBreak;

    /** @brief H at `position` recomputed from the stored records, when the walk reaches it. Never a recorded value. */
    [[nodiscard]] std::optional<Sha256Digest> recomputedDigestAt(std::uint64_t position) const {
        if (position == walkStartPosition)
            return walkStartDigest;
        if (position > walkStartPosition && position - walkStartPosition <= walkDigests.size())
            return walkDigests[static_cast<std::size_t>(position - walkStartPosition - 1)];
        if (!leftoverDigests.empty() || leftoverStartPosition != 0) {
            if (position == leftoverStartPosition)
                return leftoverStartDigest;
            if (position > leftoverStartPosition && position - leftoverStartPosition <= leftoverDigests.size())
                return leftoverDigests[static_cast<std::size_t>(position - leftoverStartPosition - 1)];
        }
        return std::nullopt;
    }
    /** @brief The last record present that checks; the position the walk started at when none does. */
    [[nodiscard]] std::uint64_t lastPresent() const noexcept {
        return checkedThrough;
    }
};

namespace detail {

struct Walk {
    std::vector<Sha256Digest>    digests;
    std::uint64_t                through = 0;
    Sha256Digest                 head{};
    std::optional<std::uint64_t> failedAt;
    ChainFinding                 finding      = ChainFinding::Ok;
    bool                         cannotVerify = false;
};

[[nodiscard]] inline Walk walkChain(std::string_view id, std::uint64_t startPosition, const Sha256Digest& startDigest, std::span<const StoredRecord> records) {
    AuditChainVerifier verifier{id, startPosition, startDigest};
    Walk               walk;
    for (const auto& record : records) {
        const ChainFinding finding = verifier.check(record.bytes, record.digest);
        if (finding == ChainFinding::Ok) {
            walk.digests.push_back(verifier.headDigest());
        } else if (finding != ChainFinding::UnsupportedVersion) {
            walk.finding = finding;
            break;
        }
    }
    walk.through      = verifier.verifiedThrough();
    walk.head         = verifier.headDigest();
    walk.failedAt     = verifier.failedAt();
    walk.cannotVerify = verifier.cannotVerify();
    return walk;
}

}  // namespace detail

/**
 * @brief Read one stream against the trims the ledgers recorded for it (10.4, 10.6) and walk its chain from the right start value.
 *
 * `trims` are the valid trims of this stream, from every ledger. Nothing here reaches the provider: the walk is the stored records and the ledgers' own words.
 */
[[nodiscard]] inline StreamEvaluation evaluateStream(const LogImage& image, std::string_view id, std::vector<TrimRecord> trims) {
    StreamEvaluation ev;
    ev.id = std::string{id};
    std::ranges::sort(trims, {}, &TrimRecord::position);
    ev.trims = std::move(trims);
    if (!ev.trims.empty())
        ev.trim = ev.trims.back();
    const StreamImage* stream = image.find(id);
    if (stream == nullptr || stream->segments.empty()) {
        ev.disposition = ev.trim.has_value() ? StreamDisposition::Removed : StreamDisposition::Absent;
        return ev;
    }
    ev.inImage                               = true;
    ev.firstPresent                          = stream->segments.front().firstSequence;
    ev.recordCount                           = stream->records.size();
    ev.segmentCount                          = stream->segments.size();
    const std::uint64_t                 k    = ev.firstPresent;
    const TrimRecord                    trim = ev.trim.value_or(TrimRecord{});
    const std::span<const StoredRecord> records{stream->records};

    // How many leading segments end at or before `q`: the whole segments an interrupted removal left.
    const auto leftoverSegments = [&](std::uint64_t q) {
        std::size_t count = 0;
        while (count < stream->segments.size() && stream->segments[count].recordCount != 0 && stream->segments[count].lastSequence() <= q)
            ++count;
        return count;
    };
    const auto leftoverRecords = [&](std::size_t segments) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < segments; ++i)
            count += stream->segments[i].recordCount;
        return count;
    };

    if (!ev.trim.has_value()) {
        ev.disposition = k == 1 ? StreamDisposition::Whole : StreamDisposition::PrefixMissing;
    } else {
        const std::uint64_t q = trim.position;
        if (k == 1)
            ev.disposition = StreamDisposition::RemovalNotCarriedOut;
        else if (k == q + 1)
            ev.disposition = StreamDisposition::Rotated;
        else if (k <= q)
            ev.disposition = leftoverSegments(q) != 0 ? StreamDisposition::InterruptedRemoval : StreamDisposition::TrimNotOnSegmentBoundary;
        else
            ev.disposition = StreamDisposition::GapAfterTrim;
    }

    std::span<const StoredRecord> walked = records;
    switch (ev.disposition) {
        case StreamDisposition::Whole:
        case StreamDisposition::RemovalNotCarriedOut:
            break;
        case StreamDisposition::Rotated:
            ev.start = {.afterSequence = trim.position, .afterDigest = trim.digest};
            break;
        case StreamDisposition::InterruptedRemoval: {
            const std::uint64_t q    = trim.position;
            ev.skipSegments          = leftoverSegments(q);
            ev.skipRecords           = leftoverRecords(ev.skipSegments);
            const auto leftover      = records.first(ev.skipRecords);
            ev.leftoverStartPosition = k;
            ev.leftoverStartDigest   = leftover.front().digest;
            const auto walk          = detail::walkChain(id, k, leftover.front().digest, leftover.subspan(1));
            ev.leftoverDigests       = walk.digests;
            ev.leftoverChecks        = walk.finding == ChainFinding::Ok && !walk.cannotVerify && walk.through == q && walk.head == trim.digest;
            ev.start                 = {.afterSequence = q, .afterDigest = trim.digest};
            walked                   = records.subspan(ev.skipRecords);
            break;
        }
        case StreamDisposition::PrefixMissing:
        case StreamDisposition::GapAfterTrim:
        case StreamDisposition::TrimNotOnSegmentBoundary:
            // The chain cannot start: the first record's stored digest stands in for its predecessor's, and nothing before it is vouched for.
            if (!records.empty()) {
                ev.walkStartPosition = k;
                ev.walkStartDigest   = records.front().digest;
                walked               = records.subspan(1);
            }
            break;
        case StreamDisposition::Removed:
        case StreamDisposition::Absent:
            break;
    }
    if (ev.disposition != StreamDisposition::PrefixMissing && ev.disposition != StreamDisposition::GapAfterTrim
        && ev.disposition != StreamDisposition::TrimNotOnSegmentBoundary) {
        ev.walkStartPosition = ev.start.afterSequence;
        ev.walkStartDigest   = ev.start.afterDigest;
    }
    const auto walk   = detail::walkChain(id, ev.walkStartPosition, ev.walkStartDigest, walked);
    ev.walkDigests    = walk.digests;
    ev.checkedThrough = walk.through;
    ev.checkedDigest  = walk.head;
    ev.failedAt       = walk.failedAt;
    ev.finding        = walk.finding;
    ev.cannotVerify   = walk.cannotVerify;

    // A recorded trim is the adapter's own statement of the digest at its position: the stored records must reproduce it wherever they reach it.
    for (const TrimRecord& recorded : ev.trims) {
        if (const auto got = ev.recomputedDigestAt(recorded.position); got.has_value() && *got != recorded.digest)
            ev.trimDigestOk = false;
    }
    if (ev.disposition == StreamDisposition::RemovalNotCarriedOut && trim.position > ev.checkedThrough && !ev.failedAt.has_value())
        ev.trimExceedsRecords = true;
    ev.layoutBreak = AnchorVerifier::firstBoundaryBreak(image.layoutOf(*stream, ev.skipSegments), ev.start);
    return ev;
}

/** @brief A suspect stream or ledger the adapter reports through the audit health signal, never as an audit event (9.6, 10.3). */
enum class IntegrityFaultKind : std::uint8_t {
    /** @brief A recomputed digest differs from a stored one. */
    ChainDigestMismatch,
    /** @brief The continuity check of 7.3 failed against the provider's anchor. */
    ContinuityFailed,
    /** @brief A `close` record cites a position or digest the stored records do not reproduce. */
    CloseCitationMismatch,
    /** @brief Two ledgers are cited by no other. */
    LedgersFork,
    /** @brief The log holds records of a stream no ledger opened. */
    StreamNotOpened,
    /** @brief The provider holds an anchor for a stream the log neither holds nor accounts for with a trim. */
    AnchoredEvidenceGone,
    LedgerRecordMalformed,
    /** @brief Segment headers and records disagree, or a trim does not end on a segment boundary. */
    StructureBroken,
    /** @brief Records are missing at the start of the stream or after its trim, or a trim cites records that are gone. */
    RecordsMissing,
    /** @brief A recorded trim's digest differs from the one the records reproduce, or an interrupted removal does not reach it. */
    TrimDigestMismatch,
    /** @brief The stream's records are in a version the library cannot decode: nothing was recomputed. */
    Unverifiable,
    /** @brief The provider refused a fresh claim as `positionNotIncreasing` or `conflict`: the log or the provider has diverged (7.3). */
    AnchorDiverged,
    /** @brief A recorded whole-stream trim awaits its `retire`, but no anchor of the ledger covers the trim, so the adapter does not relay it (7.2). */
    RetirementNotAuthorized
};

struct IntegrityFault {
    IntegrityFaultKind kind = IntegrityFaultKind::ChainDigestMismatch;
    std::string        stream;
    std::uint64_t      position = 0;
};

/** @brief The continuity check of 7.3 against the provider's latest anchor of a stream. */
enum class Continuity : std::uint8_t {
    /** @brief No provider, no anchor, or the provider did not answer. */
    NotChecked,
    Passed,
    Failed,
    /** @brief A trim went past the anchor, so H_p cannot be recomputed (10.4). The reader reports it as Cannot verify. */
    NotPossible
};

/** @brief What a restart recovered of one held stream (10.3): the last position up to which every stored digest matches, provided the continuity check holds.
 */
struct RecoveredStream {
    std::string                  id;
    bool                         ledger     = false;
    bool                         consistent = false;
    std::optional<std::uint64_t> position;
    std::optional<Sha256Digest>  digest;
    Continuity                   continuity  = Continuity::NotChecked;
    StreamDisposition            disposition = StreamDisposition::Whole;
};

/** @brief A removed stream whose provider anchor was never replaced by a retirement, and that the adapter may now retire (7.3, 10.5). */
struct PendingRetirement {
    std::string   stream;
    std::uint64_t position = 0;
};

/** @brief What the adapter concludes at start, before a new ledger writes a record (10.3). */
struct ChainStateRecovery {
    std::map<std::string, RecoveredStream, std::less<>> held;
    /** @brief The ledger record 1 will cite: the newest ledger, and under a fork the uncited one whose identity sorts last. */
    std::optional<std::string> predecessor;
    bool                       fork = false;
    /** @brief Streams the predecessor opened that the log still holds, in the order it opened them: the `recovered` records to write. */
    std::vector<std::string>       toCite;
    std::vector<IntegrityFault>    faults;
    std::vector<PendingRetirement> pendingRetirements;

    [[nodiscard]] bool absent() const noexcept {
        return !predecessor.has_value();
    }
};

/** @brief The log read as a whole: every stream and ledger, the trims the ledgers record, and how the ledgers cite one another (10.1). */
class LogAnalysis {
public:
    [[nodiscard]] static LogAnalysis read(StorageMedium& medium) {
        LogAnalysis out;
        out.pictured = LogImage::read(medium);
        out.readLedgers();
        out.evaluate();
        return out;
    }

    [[nodiscard]] const LogImage& image() const noexcept {
        return pictured;
    }
    [[nodiscard]] const std::vector<LedgerImage>& ledgers() const noexcept {
        return ledgerList;
    }
    /** @brief The ledgers in the order they were started: a ledger comes after the ledgers its chain of `predecessor` citations reaches in the log. */
    [[nodiscard]] std::vector<const LedgerImage*> ledgersOldestFirst() const {
        std::vector<std::pair<std::size_t, const LedgerImage*>> depth;
        for (const LedgerImage& item : ledgerList) {
            std::size_t        steps  = 0;
            const LedgerImage* cursor = &item;
            while (cursor->predecessor.has_value() && steps <= ledgerList.size()) {
                const LedgerImage* earlier = ledger(cursor->predecessor->target);
                if (earlier == nullptr)
                    break;
                cursor = earlier;
                ++steps;
            }
            depth.emplace_back(steps, &item);
        }
        std::ranges::stable_sort(depth, {}, &std::pair<std::size_t, const LedgerImage*>::first);
        std::vector<const LedgerImage*> out;
        out.reserve(depth.size());
        for (const auto& entry : depth)
            out.push_back(entry.second);
        return out;
    }
    [[nodiscard]] const LedgerImage* ledger(std::string_view id) const {
        const auto found = std::ranges::find(ledgerList, id, &LedgerImage::id);
        return found == ledgerList.end() ? nullptr : &*found;
    }
    [[nodiscard]] bool isLedger(std::string_view id) const {
        return ledger(id) != nullptr;
    }
    /** @brief Every stream the image holds, and every stream a valid trim names. */
    [[nodiscard]] const std::map<std::string, StreamEvaluation, std::less<>>& streams() const noexcept {
        return evaluated;
    }
    [[nodiscard]] const StreamEvaluation* evaluation(std::string_view id) const {
        const auto found = evaluated.find(id);
        return found == evaluated.end() ? nullptr : &found->second;
    }
    /** @brief Ledgers no other ledger cites as its predecessor. One is the newest ledger; two or more are a fork (10.3). */
    [[nodiscard]] const std::vector<std::string>& uncitedLedgers() const noexcept {
        return uncited;
    }
    /** @brief The newest ledger when there is exactly one uncited ledger. */
    [[nodiscard]] std::optional<std::string> newestLedger() const {
        return uncited.size() == 1 ? std::optional<std::string>{uncited.front()} : std::nullopt;
    }
    /** @brief The ledger that opened a stream: the first, in ledger order, that records an `open` for it. */
    [[nodiscard]] const LedgerImage* openedBy(std::string_view stream) const {
        for (const LedgerImage& ledgerImage : ledgerList) {
            if (std::ranges::find(ledgerImage.opened, stream) != ledgerImage.opened.end())
                return &ledgerImage;
        }
        return nullptr;
    }
    /** @brief Streams of the image that are neither a ledger nor opened by any ledger (10.3, inconsistent state). */
    [[nodiscard]] const std::vector<std::string>& streamsNoLedgerOpened() const noexcept {
        return unopened;
    }
    /** @brief A reserved action in a stream that is not a ledger: the stream and the first sequence (10.1). A reader's safeguard only. */
    [[nodiscard]] const std::vector<std::pair<std::string, std::uint64_t>>& reservedOutsideLedger() const noexcept {
        return reservedOutside;
    }
    /** @brief Streams the log holds, ledgers included. */
    [[nodiscard]] std::vector<std::string> heldStreams() const {
        std::vector<std::string> out;
        for (const auto& [id, stream] : pictured.streams())
            out.push_back(id);
        return out;
    }

private:
    void readLedgers() {
        for (const auto& [id, stream] : pictured.streams()) {
            if (auto ledgerImage = readLedger(id, stream); ledgerImage.has_value())
                ledgerList.push_back(std::move(*ledgerImage));
            else
                scanReserved(id, stream);
        }
        std::ranges::sort(ledgerList, {}, &LedgerImage::id);
        std::set<std::string> cited;
        for (const LedgerImage& ledgerImage : ledgerList) {
            if (ledgerImage.predecessor.has_value())
                cited.insert(ledgerImage.predecessor->target);
        }
        for (const LedgerImage& ledgerImage : ledgerList) {
            if (!cited.contains(ledgerImage.id))
                uncited.push_back(ledgerImage.id);
        }
        std::set<std::string> opened;
        for (const LedgerImage& ledgerImage : ledgerList)
            opened.insert(ledgerImage.opened.begin(), ledgerImage.opened.end());
        for (const auto& [id, stream] : pictured.streams()) {
            if (!isLedger(id) && !opened.contains(id) && !stream.records.empty())
                unopened.push_back(id);
        }
    }

    [[nodiscard]] static std::optional<LedgerImage> readLedger(const std::string& id, const StreamImage& stream) {
        if (stream.records.empty() || stream.segments.front().firstSequence != 1)
            return std::nullopt;
        const auto first = decodeCanonical(stream.records.front().bytes);
        if (first.status != CanonicalReadStatus::Ok || !isLedgerHeadAction(first.record.action) || first.record.sequence != 1)
            return std::nullopt;
        LedgerImage out;
        out.id              = id;
        out.head            = first.record.action == ledgeraction::origin ? LedgerRecordKind::Origin : LedgerRecordKind::Predecessor;
        out.recordsPresent  = stream.records.size();
        const auto walk     = detail::walkChain(id, 0, chainInitialValue, stream.records);
        out.verifiedThrough = walk.through;
        out.headDigest      = walk.head;
        out.chainBroken     = walk.finding != ChainFinding::Ok || walk.cannotVerify || walk.through != stream.records.size();
        LedgerChecker checker{id};
        for (std::uint64_t index = 0; index < walk.through; ++index) {
            const auto read = decodeCanonical(stream.records[static_cast<std::size_t>(index)].bytes);
            if (read.status != CanonicalReadStatus::Ok)
                break;
            const std::uint64_t sequence = read.record.sequence;
            const LedgerCheck   verdict  = checker.check(read.record);
            if (verdict.fault.has_value()) {
                out.faults.push_back({.sequence = sequence, .fault = verdict.fault.value_or(LedgerFault::UnknownReservedAction)});
                continue;
            }
            if (!verdict.view.has_value())
                continue;
            const LedgerRecordView& view = *verdict.view;
            const std::uint64_t     at   = view.sourceSequence.value_or(0);
            const Sha256Digest      cite = view.digest.value_or(Sha256Digest{});
            switch (view.kind) {
                case LedgerRecordKind::Origin:
                    break;
                case LedgerRecordKind::Predecessor:
                    out.predecessor = Citation{.target         = std::string{view.target},
                                               .checks         = view.phase == core::AuditPhase::Executed,
                                               .position       = view.sourceSequence,
                                               .digest         = view.digest,
                                               .ledgerSequence = sequence};
                    break;
                case LedgerRecordKind::Recovered:
                    out.recovered.push_back({.target         = std::string{view.target},
                                             .checks         = view.phase == core::AuditPhase::Executed,
                                             .position       = view.sourceSequence,
                                             .digest         = view.digest,
                                             .ledgerSequence = sequence});
                    break;
                case LedgerRecordKind::StreamOpen:
                    out.opened.emplace_back(view.target);
                    break;
                case LedgerRecordKind::StreamClose:
                    out.closes[std::string{view.target}] = {.position = at, .digest = cite, .ledgerSequence = sequence};
                    break;
                case LedgerRecordKind::StreamTrim:
                    // An invalid trim accounts for nothing (10.2); so does one for no record.
                    if (at >= 1)
                        out.trims.push_back({.stream = std::string{view.target}, .position = at, .digest = cite, .ledger = id, .ledgerSequence = sequence});
                    break;
                case LedgerRecordKind::LedgerClose:
                    out.closedOrderly = true;
                    break;
            }
        }
        return out;
    }

    void scanReserved(const std::string& id, const StreamImage& stream) {
        for (const StoredRecord& record : stream.records) {
            const auto read = decodeCanonical(record.bytes);
            if (read.status == CanonicalReadStatus::Ok && core::isReservedAuditAction(read.record.action)) {
                reservedOutside.emplace_back(id, read.record.sequence);
                return;
            }
        }
    }

    void evaluate() {
        std::map<std::string, std::vector<TrimRecord>, std::less<>> trims;
        for (const LedgerImage& ledgerImage : ledgerList) {
            for (const TrimRecord& trim : ledgerImage.trims)
                trims[trim.stream].push_back(trim);
        }
        std::set<std::string> names;
        for (const auto& [id, stream] : pictured.streams())
            names.insert(id);
        for (const auto& [id, list] : trims)
            names.insert(id);
        for (const std::string& id : names) {
            std::vector<TrimRecord> mine;
            if (const auto found = trims.find(id); found != trims.end())
                mine = found->second;
            evaluated.emplace(id, evaluateStream(pictured, id, std::move(mine)));
        }
    }

    LogImage                                             pictured;
    std::vector<LedgerImage>                             ledgerList;
    std::map<std::string, StreamEvaluation, std::less<>> evaluated;
    std::vector<std::string>                             uncited;
    std::vector<std::string>                             unopened;
    std::vector<std::pair<std::string, std::uint64_t>>   reservedOutside;
};

namespace detail {

[[nodiscard]] inline const Anchor* anchorIn(const LatestAnswer& answer) noexcept {
    if (const auto* anchor = std::get_if<Anchor>(&answer))
        return anchor;
    if (const auto* retirement = std::get_if<Retirement>(&answer))
        return &retirement->finalAnchor;
    return nullptr;
}

}  // namespace detail

/**
 * @brief Recover chain state from the stored records (10.3): for every stream, the last position up to which every stored digest matches, and, where the
 * provider holds an anchor, the continuity check of 7.3.
 *
 * Nothing is repaired, rewritten or reordered. The result says what record 1 and the `recovered` records may cite, and what the adapter must report as an
 * integrity fault. `provider` may be null.
 */
[[nodiscard]] inline ChainStateRecovery recoverChainState(const LogAnalysis& log, AnchorProvider* provider) {
    ChainStateRecovery out;
    const auto         fault = [&](IntegrityFaultKind kind, std::string stream, std::uint64_t position = 0) {
        out.faults.push_back({.kind = kind, .stream = std::move(stream), .position = position});
    };

    for (const auto& [id, ev] : log.streams()) {
        if (!ev.inImage)
            continue;
        const LedgerImage* ledgerImage = log.ledger(id);
        RecoveredStream    state;
        state.id          = id;
        state.ledger      = ledgerImage != nullptr;
        state.disposition = ev.disposition;
        bool sound        = true;

        if (ev.finding != ChainFinding::Ok) {
            sound = false;
            fault(IntegrityFaultKind::ChainDigestMismatch, id, ev.failedAt.value_or(0));
        }
        if (ev.cannotVerify) {
            sound = false;
            fault(IntegrityFaultKind::Unverifiable, id);
        }
        switch (ev.disposition) {
            case StreamDisposition::PrefixMissing:
            case StreamDisposition::GapAfterTrim:
                sound = false;
                fault(IntegrityFaultKind::RecordsMissing, id, ev.firstPresent);
                break;
            case StreamDisposition::TrimNotOnSegmentBoundary:
                sound = false;
                fault(IntegrityFaultKind::StructureBroken, id, ev.trim.value_or(TrimRecord{}).position);
                break;
            default:
                break;
        }
        if (ev.trimExceedsRecords) {
            sound = false;
            fault(IntegrityFaultKind::RecordsMissing, id, ev.trim.value_or(TrimRecord{}).position);
        }
        if (!ev.trimDigestOk || !ev.leftoverChecks) {
            sound = false;
            fault(IntegrityFaultKind::TrimDigestMismatch, id, ev.trim.value_or(TrimRecord{}).position);
        }
        if (ev.layoutBreak.has_value()) {
            sound = false;
            fault(IntegrityFaultKind::StructureBroken, id, *ev.layoutBreak);
        }
        if (ledgerImage != nullptr && !ledgerImage->faults.empty()) {
            sound = false;
            fault(IntegrityFaultKind::LedgerRecordMalformed, id, ledgerImage->faults.front().sequence);
        }

        // The continuity check (7.3): H_p recomputed from the stored records, against what the provider holds.
        if (provider != nullptr && ev.inImage) {
            const LatestAnswer answer = provider->latest(id);
            if (const Anchor* anchor = detail::anchorIn(answer); anchor != nullptr && anchor->usable()) {
                const auto got = ev.recomputedDigestAt(anchor->position);
                if (ev.start.afterSequence > anchor->position) {
                    state.continuity = Continuity::NotPossible;
                } else if (got.has_value() && *got == anchor->digest) {
                    state.continuity = Continuity::Passed;
                } else {
                    // Whatever the walk reached either differs, or stops short of the anchor: the anchored evidence is not what the log holds.
                    state.continuity = Continuity::Failed;
                    sound            = false;
                    fault(IntegrityFaultKind::ContinuityFailed, id, anchor->position);
                }
            }
        }
        state.consistent = sound;
        // The last position that checks, when the walk checked at least one record; none is recovered if the continuity check failed.
        if (state.continuity != Continuity::Failed && (ev.checkedThrough > ev.walkStartPosition || (sound && ev.recordCount > 0))) {
            state.position = ev.checkedThrough;
            state.digest   = ev.checkedDigest;
        }
        out.held.emplace(id, std::move(state));
    }

    // `close` records cite positions and digests the stored records must reproduce (10.3).
    for (const LedgerImage& ledgerImage : log.ledgers()) {
        for (const auto& [stream, close] : ledgerImage.closes) {
            const StreamEvaluation* ev = log.evaluation(stream);
            if (ev == nullptr || !ev->inImage)
                continue;
            if (ev->trim.has_value() && ev->trim.value_or(TrimRecord{}).position >= close.position)
                continue;  // those records were removed under retention
            const auto got = ev->recomputedDigestAt(close.position);
            if (!got.has_value() || *got != close.digest) {
                auto& held      = out.held[stream];
                held.id         = stream;
                held.consistent = false;
                fault(IntegrityFaultKind::CloseCitationMismatch, stream, close.position);
            }
        }
    }

    for (const auto& id : log.streamsNoLedgerOpened()) {
        out.held[id].consistent = false;
        fault(IntegrityFaultKind::StreamNotOpened, id);
    }

    const auto& uncited = log.uncitedLedgers();
    if (uncited.size() >= 2) {
        out.fork = true;
        for (const auto& id : uncited) {
            fault(IntegrityFaultKind::LedgersFork, id);
            out.held[id].consistent = false;
        }
    }
    if (!uncited.empty())
        out.predecessor = *std::ranges::max_element(uncited);

    if (out.predecessor.has_value()) {
        if (const LedgerImage* earlier = log.ledger(*out.predecessor); earlier != nullptr) {
            for (const auto& stream : earlier->opened) {
                const StreamEvaluation* ev = log.evaluation(stream);
                if (ev == nullptr || !ev->inImage || ev->recordCount == 0 || log.isLedger(stream))
                    continue;
                if (std::ranges::find(out.toCite, stream) == out.toCite.end())
                    out.toCite.push_back(stream);
            }
        }
    }

    // The provider's view: streams it holds that the log neither holds nor accounts for with a trim are anchored evidence that has gone.
    if (provider != nullptr) {
        const StreamsAnswer listing = provider->streams();
        if (const auto* all = std::get_if<ProviderListing>(&listing)) {
            for (const StreamEntry& entry : all->entries) {
                const Anchor&           anchor  = std::holds_alternative<Anchor>(entry) ? std::get<Anchor>(entry) : std::get<Retirement>(entry).finalAnchor;
                const bool              retired = std::holds_alternative<Retirement>(entry);
                const StreamEvaluation* ev      = log.evaluation(anchor.streamId);
                const bool              held    = ev != nullptr && ev->inImage;
                const bool              trimmed = ev != nullptr && ev->trim.has_value();
                if (!held && !trimmed) {
                    if (!retired)
                        fault(IntegrityFaultKind::AnchoredEvidenceGone, anchor.streamId, anchor.position);
                    continue;
                }
                if (held || retired)
                    continue;
                // Removed under a trim and still anchored: the retirement is owed (7.3, 10.5), but only on the word of a ledger an anchor covers.
                const TrimRecord&  trim      = *ev->trim;
                const LedgerImage* recording = log.ledger(trim.ledger);
                bool               covered   = false;
                if (recording != nullptr) {
                    const LatestAnswer ledgerAnswer = provider->latest(trim.ledger);
                    if (const Anchor* ledgerAnchor = detail::anchorIn(ledgerAnswer);
                        ledgerAnchor != nullptr && ledgerAnchor->usable() && ledgerAnchor->position >= trim.ledgerSequence) {
                        const StreamEvaluation* ledgerEv = log.evaluation(trim.ledger);
                        const auto              got      = ledgerEv != nullptr ? ledgerEv->recomputedDigestAt(ledgerAnchor->position) : std::nullopt;
                        covered                          = got.has_value() && *got == ledgerAnchor->digest;
                    }
                }
                if (covered)
                    out.pendingRetirements.push_back({.stream = anchor.streamId, .position = anchor.position});
                else
                    fault(IntegrityFaultKind::RetirementNotAuthorized, anchor.streamId, trim.position);
            }
        }
    }
    return out;
}

}  // namespace mddlog::adapter
