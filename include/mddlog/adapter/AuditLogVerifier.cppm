/** @brief The log reader: per-stream verdicts, and what the ledger records at each boundary (ADR-004 10.6). Adapter zone only. */

export module mddlog.adapter.auditlogverifier;

import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditlog;

export namespace mddlog::adapter {

/** @brief One boundary, or one finding about a boundary, that the ledger lets a reader state (10.6). Positions and identities are in `BoundaryNote`. */
enum class BoundaryKind : std::uint8_t {
    /** @brief `predecessor` cites an earlier ledger and the stored records of that ledger reproduce the citation, or the provider's retirement does. */
    PredecessorMatched,
    /** @brief The `Executed` form cites something the records do not reproduce: the evidence changed after the restart. */
    PredecessorNotReproduced,
    /** @brief The earlier ledger is gone and no retirement says what it held: the link is no longer checkable. */
    PredecessorNotCheckable,
    /** @brief `predecessor` in its `Failed` form: the earlier ledger did not check at restart. `position` is the last position that did, 0 when none. */
    PredecessorFailedForm,
    RecoveredMatched,
    RecoveredNotReproduced,
    RecoveredNotCheckable,
    RecoveredFailedForm,
    /** @brief `close` is recorded, and the stream's last record and its digest match it. */
    ClosedAt,
    /** @brief `close` is recorded and the stored records do not reproduce it. */
    ClosedCitationNotReproduced,
    /** @brief The stream's ledger is not the newest and records no `close` for it. Records after `position` may have been lost before confirmation or removed.
     */
    EndedWithoutClose,
    /** @brief The newest ledger records no `close` for the stream yet: it may still be live, or the session may have ended abruptly. */
    NoCloseRecorded,
    /** @brief The ledger records no `ledger.close`. */
    LedgerNotClosed,
    /** @brief Records `1 … position` were removed under retention, recorded at the ledger position `ledgerPosition`. */
    RemovedUnderRetention,
    /** @brief A whole stream removed under retention that the provider never anchored, or without a provider. */
    RemovedNeverAnchored,
    /** @brief Records `position … secondPosition` of a removed stream were never anchored: the trim is the only trace of them. */
    RemovedWithoutAnchor,
    /** @brief Records `position … secondPosition` are left over from an interrupted removal and reach the trim's digest. */
    LeftoverFromInterruptedRemoval,
    /** @brief A trim is recorded and every record is still present: the removal did not happen. */
    RemovalNotCarriedOut,
    /** @brief Records `1 … position - 1` are missing and no trim accounts for them. */
    PrefixMissingWithoutTrim,
    /**
     * @brief Records `position … secondPosition` are missing between a trim and the first record present, or, for a removed stream, between its highest trim
     * and the last position a ledger or the provider's retirement cites.
     */
    RecordsMissingAfterTrim,
    /** @brief A trim past the anchor leaves records of the stream: `H_p` can no longer be recomputed. */
    TrimPastAnchor,
    /** @brief The newest ledger begins with `origin` while the provider or the retained position knows streams the log does not hold. */
    OriginClaimedWhileHistoryExists,
    /** @brief The ledger begins with `origin` and nothing known contradicts it. A first commissioning and a complete loss cannot be told apart. */
    NoEarlierHistoryKnown,
    /** @brief Two ledgers are cited by no other. */
    LedgersFork,
    /** @brief The ledger lies on a cycle of `predecessor` citations, whether or not another ledger is uncited. */
    LedgerCitationCycle,
    /** @brief The log holds records of a stream no ledger opened. */
    StreamNotOpenedByAnyLedger,
    /** @brief A reserved action in a stream that is not a ledger, at the sequence `position`. */
    ReservedActionOutsideLedger,
    /** @brief A ledger record is malformed, at the sequence `position`. */
    LedgerRecordMalformed
};

[[nodiscard]] constexpr std::string_view boundaryName(BoundaryKind kind) noexcept {
    switch (kind) {
        case BoundaryKind::PredecessorMatched:
            return "predecessor cited, matched";
        case BoundaryKind::PredecessorNotReproduced:
            return "predecessor cited, not reproduced: the evidence changed after the restart";
        case BoundaryKind::PredecessorNotCheckable:
            return "predecessor cited, no longer checkable";
        case BoundaryKind::PredecessorFailedForm:
            return "predecessor cited as not checking";
        case BoundaryKind::RecoveredMatched:
            return "recovered stream cited, matched";
        case BoundaryKind::RecoveredNotReproduced:
            return "recovered stream cited, not reproduced: the evidence changed after the restart";
        case BoundaryKind::RecoveredNotCheckable:
            return "recovered stream cited, no longer checkable";
        case BoundaryKind::RecoveredFailedForm:
            return "recovered stream cited as not checking";
        case BoundaryKind::ClosedAt:
            return "closed at";
        case BoundaryKind::ClosedCitationNotReproduced:
            return "close recorded, not reproduced by the records";
        case BoundaryKind::EndedWithoutClose:
            return "ended without close at";
        case BoundaryKind::NoCloseRecorded:
            return "no close recorded, last record at";
        case BoundaryKind::LedgerNotClosed:
            return "ledger records no close";
        case BoundaryKind::RemovedUnderRetention:
            return "records removed under retention through";
        case BoundaryKind::RemovedNeverAnchored:
            return "removed under retention, never anchored";
        case BoundaryKind::RemovedWithoutAnchor:
            return "removed without anchor";
        case BoundaryKind::LeftoverFromInterruptedRemoval:
            return "left over from an interrupted removal";
        case BoundaryKind::RemovalNotCarriedOut:
            return "trim recorded, removal did not happen";
        case BoundaryKind::PrefixMissingWithoutTrim:
            return "records missing at the start, no trim accounts for them";
        case BoundaryKind::RecordsMissingAfterTrim:
            return "records missing after the trim";
        case BoundaryKind::TrimPastAnchor:
            return "trim past the anchor";
        case BoundaryKind::OriginClaimedWhileHistoryExists:
            return "origin claimed while earlier history exists";
        case BoundaryKind::NoEarlierHistoryKnown:
            return "no earlier history known";
        case BoundaryKind::LedgersFork:
            return "ledgers fork";
        case BoundaryKind::LedgerCitationCycle:
            return "ledger citations form a cycle";
        case BoundaryKind::StreamNotOpenedByAnyLedger:
            return "stream opened by no ledger";
        case BoundaryKind::ReservedActionOutsideLedger:
            return "reserved action outside a ledger";
        case BoundaryKind::LedgerRecordMalformed:
            return "ledger record malformed";
    }
    return "";
}

/** @brief One boundary a reader states. Which fields carry a value depends on the kind, as each kind's comment says. */
struct BoundaryNote {
    BoundaryKind kind = BoundaryKind::NoEarlierHistoryKnown;
    /** @brief The stream or ledger the note is about. */
    std::string stream;
    /** @brief The other stream a citation names, when it differs. */
    std::string   other;
    std::uint64_t position       = 0;
    std::uint64_t secondPosition = 0;
    /** @brief The ledger that recorded the boundary, and the position of the record. */
    std::string   ledger;
    std::uint64_t ledgerPosition = 0;
};

/** @brief Build a note; the fields a kind does not use stay empty. */
[[nodiscard]] inline BoundaryNote makeBoundaryNote(BoundaryKind  kind,
                                                   std::string   stream,
                                                   std::uint64_t position       = 0,
                                                   std::uint64_t secondPosition = 0,
                                                   std::string   ledger         = {},
                                                   std::uint64_t ledgerPosition = 0,
                                                   std::string   other          = {}) {
    BoundaryNote note;
    note.kind           = kind;
    note.stream         = std::move(stream);
    note.other          = std::move(other);
    note.position       = position;
    note.secondPosition = secondPosition;
    note.ledger         = std::move(ledger);
    note.ledgerPosition = ledgerPosition;
    return note;
}

/** @brief The 7.5 report of one stream or ledger, with how its records stand against what the ledgers recorded about its prefix. */
struct StreamBoundaryReport {
    StreamReport      report;
    bool              ledger      = false;
    StreamDisposition disposition = StreamDisposition::Whole;
};

/** @brief What a reader reports for a whole log: per stream, then the boundaries. Nothing is folded into one "valid" result (7.5, 10.6). */
struct LogReport {
    std::vector<StreamBoundaryReport> streams;
    /** @brief Streams the provider or the retained position knows, that the log holds nothing of and no trim accounts for: Incomplete with m = 0 (10.6). */
    std::vector<StreamReport> unlisted;
    std::vector<BoundaryNote> notes;
    /** @brief The medium could not be listed or a segment could not be read: records may be missing for that reason alone. */
    bool mediumUnreadable = false;

    [[nodiscard]] const StreamBoundaryReport* find(std::string_view id) const {
        const auto found = std::ranges::find_if(streams, [&](const StreamBoundaryReport& item) {
            return item.report.streamId == id;
        });
        return found == streams.end() ? nullptr : &*found;
    }
    [[nodiscard]] const StreamReport* findUnlisted(std::string_view id) const {
        const auto found = std::ranges::find(unlisted, id, &StreamReport::streamId);
        return found == unlisted.end() ? nullptr : &*found;
    }
    /** @brief Every note of one kind, in the order they were stated. */
    [[nodiscard]] std::vector<BoundaryNote> notesOf(BoundaryKind kind) const {
        std::vector<BoundaryNote> out;
        std::ranges::copy_if(notes, std::back_inserter(out), [&](const BoundaryNote& note) {
            return note.kind == kind;
        });
        return out;
    }
    [[nodiscard]] bool has(BoundaryKind kind, std::string_view stream) const {
        return std::ranges::any_of(notes, [&](const BoundaryNote& note) {
            return note.kind == kind && note.stream == stream;
        });
    }
};

/**
 * @brief Verifies a whole log: every stream and ledger against the provider's anchors and the retained position (7.5), and the boundaries the ledgers record
 * (10.6).
 *
 * Holds references: the medium, the provider and the retained position must outlive it. The ledgers are read like any stream, and their words only say what the
 * adapter recorded, never what an anchor covers (10.1).
 */
class LogVerifier {
public:
    LogVerifier(StorageMedium& storage, AnchorProvider& anchorProvider, RetainedPosition& retainedPosition, VerifierConfig verifierConfig = {}) noexcept
        : medium(&storage), provider(&anchorProvider), retained(&retainedPosition), config(verifierConfig) {}

    [[nodiscard]] LogReport verify() {
        const LogAnalysis log = LogAnalysis::read(*medium);
        LogReport         out;
        out.mediumUnreadable = log.image().unreadable();
        AnchorVerifier anchors{*provider, *retained, config};

        for (const auto& [id, ev] : log.streams())
            out.streams.push_back(verifyOne(log, ev, anchors, out.notes));

        std::vector<std::string> accounted;
        for (const auto& [id, ev] : log.streams())
            accounted.push_back(id);
        out.unlisted = anchors.verifyUnlisted(accounted);

        boundaries(log, out);
        return out;
    }

private:
    [[nodiscard]] static StreamReport shell(const StreamEvaluation& ev, Verdict verdict, VerdictCause cause) {
        StreamReport report;
        report.streamId      = ev.id;
        report.verdict       = verdict;
        report.cause         = cause;
        report.firstRetained = ev.disposition == StreamDisposition::Rotated ? ev.trim.value_or(TrimRecord{}).position + 1 : ev.firstPresent;
        report.lastPresent   = ev.checkedThrough;
        report.failedAt      = ev.failedAt;
        return report;
    }

    /** @brief A report the reader fixes itself because the chain cannot start from a recorded value: the records present are checked against each other only.
     */
    [[nodiscard]] static StreamReport unanchoredPrefix(const StreamEvaluation& ev, Verdict verdict, VerdictCause cause, std::optional<std::uint64_t> failedAt) {
        StreamReport report  = shell(ev, verdict, cause);
        report.firstRetained = ev.firstPresent;
        report.failedAt      = failedAt;
        return report;
    }

    [[nodiscard]] StreamBoundaryReport
    verifyOne(const LogAnalysis& log, const StreamEvaluation& ev, AnchorVerifier& anchors, std::vector<BoundaryNote>& notes) {
        const LedgerImage*   ledgerImage = log.ledger(ev.id);
        StreamBoundaryReport out;
        out.ledger                = ledgerImage != nullptr;
        out.disposition           = ev.disposition;
        const StreamImage* stream = log.image().find(ev.id);
        const TrimRecord   trim   = ev.trim.value_or(TrimRecord{});
        const auto         note   = [&](BoundaryKind kind, std::uint64_t position = 0, std::uint64_t second = 0) {
            notes.push_back(makeBoundaryNote(kind, ev.id, position, second, trim.ledger, trim.ledgerSequence));
        };

        switch (ev.disposition) {
            case StreamDisposition::PrefixMissing:
                out.report = ev.failedAt.has_value() ? unanchoredPrefix(ev, Verdict::Inconsistent, VerdictCause::None, ev.failedAt)
                                                     : unanchoredPrefix(ev, Verdict::Incomplete, VerdictCause::PrefixMissingWithoutTrim, std::nullopt);
                note(BoundaryKind::PrefixMissingWithoutTrim, ev.firstPresent);
                break;
            case StreamDisposition::GapAfterTrim:
                out.report = ev.failedAt.has_value() ? unanchoredPrefix(ev, Verdict::Inconsistent, VerdictCause::None, ev.failedAt)
                                                     : unanchoredPrefix(ev, Verdict::Incomplete, VerdictCause::RecordsMissingAfterTrim, std::nullopt);
                note(BoundaryKind::RemovedUnderRetention, trim.position);
                note(BoundaryKind::RecordsMissingAfterTrim, trim.position + 1, ev.firstPresent - 1);
                break;
            case StreamDisposition::TrimNotOnSegmentBoundary:
                out.report = unanchoredPrefix(ev, Verdict::Inconsistent, VerdictCause::TrimNotOnSegmentBoundary, trim.position);
                break;
            case StreamDisposition::RemovalNotCarriedOut:
                out.report = removalNotCarriedOut(log, ev, *stream, anchors);
                note(BoundaryKind::RemovalNotCarriedOut, trim.position);
                break;
            case StreamDisposition::InterruptedRemoval:
                out.report = interrupted(log, ev, *stream, anchors);
                note(BoundaryKind::LeftoverFromInterruptedRemoval, ev.firstPresent, trim.position);
                break;
            case StreamDisposition::Rotated:
                out.report = anchors.verify(ev.id, stream->records, ev.start, log.image().layoutOf(*stream));
                note(BoundaryKind::RemovedUnderRetention, trim.position);
                if (out.report.cause == VerdictCause::TrimPastAnchor)
                    note(BoundaryKind::TrimPastAnchor, trim.position);
                break;
            case StreamDisposition::Whole:
                out.report = anchors.verify(ev.id, stream->records, {}, log.image().layoutOf(*stream));
                break;
            case StreamDisposition::Removed:
                out.report = removed(log, ev, anchors, notes);
                break;
            case StreamDisposition::Absent:
                out.report = anchors.verify(ev.id, {});
                break;
        }
        if (ledgerImage != nullptr && !ledgerImage->faults.empty() && out.report.verdict != Verdict::Inconsistent) {
            out.report.verdict  = Verdict::Inconsistent;
            out.report.cause    = VerdictCause::LedgerRecordMalformed;
            out.report.failedAt = ledgerImage->faults.front().sequence;
            out.report.anchoredThrough.reset();
            out.report.unanchoredFrom.reset();
        }
        if (ledgerImage != nullptr) {
            for (const auto& fault : ledgerImage->faults)
                notes.push_back(makeBoundaryNote(BoundaryKind::LedgerRecordMalformed, ev.id, fault.sequence));
        }
        return out;
    }

    /** @brief The trim is still recorded and no segment was reclaimed: the stream is verified from H_0 and the trim's digest is checked too (10.4). */
    [[nodiscard]] static StreamReport
    removalNotCarriedOut(const LogAnalysis& log, const StreamEvaluation& ev, const StreamImage& stream, AnchorVerifier& anchors) {
        if (!ev.trimDigestOk)
            return unanchoredPrefix(ev, Verdict::Inconsistent, VerdictCause::TrimDigestMismatch, ev.trim.value_or(TrimRecord{}).position);
        StreamReport report = anchors.verify(ev.id, stream.records, {}, log.image().layoutOf(stream));
        if (ev.trimExceedsRecords && report.verdict != Verdict::Inconsistent) {
            report.verdict = Verdict::Incomplete;
            report.cause   = VerdictCause::TrimExceedsRecords;
            report.anchoredThrough.reset();
            report.unanchoredFrom.reset();
        }
        return report;
    }

    /** @brief Records `k … q` remain: they must reach the trim's `H_q` from record `k`'s stored digest, then the stream is verified from `q + 1` (10.4). */
    [[nodiscard]] static StreamReport interrupted(const LogAnalysis& log, const StreamEvaluation& ev, const StreamImage& stream, AnchorVerifier& anchors) {
        if (!ev.leftoverChecks)
            return unanchoredPrefix(ev, Verdict::Inconsistent, VerdictCause::LeftoverDoesNotReachTrim, ev.trim.value_or(TrimRecord{}).position);
        const auto rest = std::span<const StoredRecord>{stream.records}.subspan(ev.skipRecords);
        if (rest.empty() && ev.skipSegments >= stream.segments.size())
            return anchors.verify(ev.id, {});
        return anchors.verify(ev.id, rest, ev.start, log.image().layoutOf(stream, ev.skipSegments));
    }

    /**
     * @brief No segment is left and a trim is recorded: the stream aged out under retention, or the removal is still waiting for its `retire` (10.5).
     *
     * A whole-stream trim has `q = m`. A retirement past `q`, or a `close` or `recovered` record citing a position past `q`, shows records that went with no
     * trim: the stream is Incomplete, never Retired on the strength of a rotation's trim.
     */
    [[nodiscard]] StreamReport removed(const LogAnalysis& log, const StreamEvaluation& ev, AnchorVerifier& anchors, std::vector<BoundaryNote>& notes) {
        const LatestAnswer latest = provider->latest(ev.id);
        const TrimRecord   trim   = ev.trim.value_or(TrimRecord{});
        const auto         note   = [&](BoundaryKind kind, std::uint64_t position, std::uint64_t second) {
            notes.push_back(makeBoundaryNote(kind, ev.id, position, second, trim.ledger, trim.ledgerSequence));
        };
        // The report the reader fixes itself when records went with no trim. The retained position is not raised on it.
        const auto missing = [&](VerdictCause cause, std::uint64_t through, const Anchor* anchor) {
            note(BoundaryKind::RecordsMissingAfterTrim, trim.position + 1, through);
            StreamReport report  = shell(ev, Verdict::Incomplete, cause);
            report.firstRetained = trim.position + 1;
            report.lastPresent   = trim.position;
            if (anchor != nullptr)
                report.anchor = *anchor;
            return report;
        };
        note(BoundaryKind::RemovedUnderRetention, trim.position, 0);
        const auto    cited  = log.highestCitedPosition(ev.id);
        const Anchor* anchor = detail::anchorIn(latest);
        if (const auto* retirement = std::get_if<Retirement>(&latest)) {
            const Anchor& finalAnchor = retirement->finalAnchor;
            if (trim.position == finalAnchor.position && trim.digest != finalAnchor.digest) {
                // The two digests are compared before anything is accepted: the retained position is not raised on a disagreement.
                StreamReport report  = shell(ev, Verdict::Altered, VerdictCause::TrimDiffersFromRetirement);
                report.firstRetained = trim.position + 1;
                report.lastPresent   = trim.position;
                report.anchor        = finalAnchor;
                return report;
            }
            if (trim.position < finalAnchor.position)
                return missing(VerdictCause::RetirementBeyondTrim, finalAnchor.position, &finalAnchor);
            if (trim.position > finalAnchor.position)
                note(BoundaryKind::RemovedWithoutAnchor, finalAnchor.position + 1, trim.position);
        } else if (std::holds_alternative<AnchorAbsent>(latest)) {
            note(BoundaryKind::RemovedNeverAnchored, trim.position, 0);
        }
        if (cited.has_value() && cited->position > trim.position)
            return missing(VerdictCause::RecordsMissingAfterTrim, cited->position, anchor);
        return anchors.verify(ev.id, {});
    }

    [[nodiscard]] static std::optional<Sha256Digest> reproduce(const LogAnalysis& log, std::string_view stream, std::uint64_t position) {
        const StreamEvaluation* ev = log.evaluation(stream);
        if (ev == nullptr || !ev->inImage)
            return std::nullopt;
        return ev->recomputedDigestAt(position);
    }

    void boundaries(const LogAnalysis& log, LogReport& out) {
        const auto newest = log.newestLedger();
        if (log.uncitedLedgers().size() >= 2) {
            for (const auto& id : log.uncitedLedgers())
                out.notes.push_back(makeBoundaryNote(BoundaryKind::LedgersFork, id));
        }
        for (const auto& id : log.cycleMembers())
            out.notes.push_back(makeBoundaryNote(BoundaryKind::LedgerCitationCycle, id));
        for (const auto& id : log.streamsNoLedgerOpened())
            out.notes.push_back(makeBoundaryNote(BoundaryKind::StreamNotOpenedByAnyLedger, id));
        for (const auto& [id, sequence] : log.reservedOutsideLedger()) {
            const StreamEvaluation* ev = log.evaluation(id);
            if (ev != nullptr && ev->disposition == StreamDisposition::InterruptedRemoval)
                continue;  // what an interrupted removal left of a ledger's first records: its own trim already says so
            out.notes.push_back(makeBoundaryNote(BoundaryKind::ReservedActionOutsideLedger, id, sequence));
        }

        for (const LedgerImage& ledgerImage : log.ledgers()) {
            if (!ledgerImage.closedOrderly)
                out.notes.push_back(makeBoundaryNote(BoundaryKind::LedgerNotClosed, ledgerImage.id));
            if (ledgerImage.head == LedgerRecordKind::Origin) {
                const bool isNewest = newest.has_value() && *newest == ledgerImage.id;
                out.notes.push_back(
                    makeBoundaryNote(isNewest && !out.unlisted.empty() ? BoundaryKind::OriginClaimedWhileHistoryExists : BoundaryKind::NoEarlierHistoryKnown,
                                     ledgerImage.id));
            }
            if (ledgerImage.predecessor.has_value())
                citePredecessor(log, ledgerImage, *ledgerImage.predecessor, out);
            for (const Citation& citation : ledgerImage.recovered)
                citeRecovered(log, ledgerImage, citation, out);
            for (const auto& stream : ledgerImage.opened)
                describeEnd(log, ledgerImage, stream, newest.has_value() && *newest == ledgerImage.id, out);
        }
    }

    void citePredecessor(const LogAnalysis& log, const LedgerImage& ledgerImage, const Citation& citation, LogReport& out) {
        BoundaryNote note = makeBoundaryNote(BoundaryKind::PredecessorFailedForm,
                                             ledgerImage.id,
                                             citation.position.value_or(0),
                                             0,
                                             ledgerImage.id,
                                             citation.ledgerSequence,
                                             citation.target);
        if (!citation.checks) {
            note.kind = BoundaryKind::PredecessorFailedForm;
            out.notes.push_back(note);
            return;
        }
        note.kind =
            compareCitation(log, citation, BoundaryKind::PredecessorMatched, BoundaryKind::PredecessorNotReproduced, BoundaryKind::PredecessorNotCheckable);
        out.notes.push_back(note);
    }

    void citeRecovered(const LogAnalysis& log, const LedgerImage& ledgerImage, const Citation& citation, LogReport& out) {
        BoundaryNote note = makeBoundaryNote(BoundaryKind::RecoveredFailedForm,
                                             ledgerImage.id,
                                             citation.position.value_or(0),
                                             0,
                                             ledgerImage.id,
                                             citation.ledgerSequence,
                                             citation.target);
        note.kind =
            citation.checks
                ? compareCitation(log, citation, BoundaryKind::RecoveredMatched, BoundaryKind::RecoveredNotReproduced, BoundaryKind::RecoveredNotCheckable)
                : BoundaryKind::RecoveredFailedForm;
        out.notes.push_back(note);
    }

    /** @brief Compare a citation with the stored records of its target, or with the provider's retirement when the target is gone (10.6). */
    [[nodiscard]] BoundaryKind
    compareCitation(const LogAnalysis& log, const Citation& citation, BoundaryKind matched, BoundaryKind notReproduced, BoundaryKind notCheckable) {
        if (!citation.position.has_value() || !citation.digest.has_value())
            return notReproduced;
        if (const StreamEvaluation* ev = log.evaluation(citation.target); ev != nullptr && ev->inImage) {
            if (const auto got = ev->recomputedDigestAt(*citation.position); got.has_value())
                return *got == *citation.digest ? matched : notReproduced;
            // Past what the walk reaches, or before the records the retention left: the trim's word is the only trace, and it is the adapter's own.
            if (ev->trim.has_value() && ev->trim.value_or(TrimRecord{}).position == *citation.position)
                return ev->trim.value_or(TrimRecord{}).digest == *citation.digest ? matched : notReproduced;
            return ev->checkedThrough < *citation.position ? notReproduced : notCheckable;
        }
        if (const auto latest = provider->latest(citation.target); const auto* retirement = std::get_if<Retirement>(&latest)) {
            const Anchor& finalAnchor = retirement->finalAnchor;
            return finalAnchor.position == *citation.position && finalAnchor.digest == *citation.digest ? matched : notReproduced;
        }
        return notCheckable;
    }

    /** @brief "Closed at m" or "ended without close at m" for a stream a ledger opened and the log still holds. */
    static void describeEnd(const LogAnalysis& log, const LedgerImage& ledgerImage, const std::string& stream, bool newest, LogReport& out) {
        const StreamEvaluation* ev = log.evaluation(stream);
        if (ev == nullptr || !ev->inImage || ev->recordCount == 0)
            return;
        const std::uint64_t last = ev->checkedThrough;
        BoundaryNote        note = makeBoundaryNote(BoundaryKind::NoCloseRecorded, stream, last, 0, ledgerImage.id);
        if (const auto close = ledgerImage.closes.find(stream); close != ledgerImage.closes.end()) {
            note.ledgerPosition = close->second.ledgerSequence;
            const auto got      = ev->recomputedDigestAt(close->second.position);
            note.kind           = close->second.position == last && got.has_value() && *got == close->second.digest ? BoundaryKind::ClosedAt
                                                                                                                    : BoundaryKind::ClosedCitationNotReproduced;
            note.position       = close->second.position;
        } else {
            note.kind = newest ? BoundaryKind::NoCloseRecorded : BoundaryKind::EndedWithoutClose;
        }
        out.notes.push_back(note);
    }

    StorageMedium*    medium;
    AnchorProvider*   provider;
    RetainedPosition* retained;
    VerifierConfig    config;
};

}  // namespace mddlog::adapter
