/** @brief Anchor-aware verifier with explicit coverage states, and the reader's retained position (ADR-004 7.4 and 7.5). Adapter zone only. */

export module mddlog.adapter.auditverifier;

import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditanchor;
export import mddlog.adapter.auditchain;

export namespace mddlog::adapter {

/** @brief One stored record as a reader holds it: its canonical bytes C and the digest stored beside it. */
struct StoredRecord {
    std::span<const std::uint8_t> bytes;
    Sha256Digest                  digest{};
};

/** @brief One segment of a stream as the reader found it: its header's numbers and how many valid record frames it held (9.4, 9.5). */
struct SegmentBoundary {
    std::uint32_t segmentIndex  = 0;
    std::uint64_t firstSequence = 0;
    std::size_t   recordCount   = 0;
};

/**
 * @brief What the stored layout contributes to a verdict, beside the records themselves (9.4, 9.5).
 *
 * A reader that does not know a segment's layout version reads nothing from it, and cannot tell which stream it belongs to, so the stream cannot be verified.
 * The segment boundaries let the verifier check that the headers and the records agree, which a bare list of records cannot show.
 */
struct StoredLayout {
    /** @brief The stream's segments in index order. Empty when the caller holds records with no segment structure, which then checks no boundary. */
    std::vector<SegmentBoundary> segments;
    /** @brief A segment on the medium has a layout version this reader does not know: nothing was read from it. */
    std::optional<std::uint16_t> unknownLayoutVersion;
    /** @brief A segment could not be read, or the medium could not be listed: records may be missing for that reason alone. */
    bool unreadable = false;
};

/** @brief Where a stream's records start: `afterSequence` 0 and H_0, or a trim's recorded `q` and `H_q` (10.4). */
struct StreamStart {
    std::uint64_t afterSequence = 0;
    Sha256Digest  afterDigest   = chainInitialValue;
};

/** @brief The verdicts of 7.5, in the order in which the first that applies is reported. */
enum class Verdict : std::uint8_t {
    Inconsistent,
    CannotVerify,
    RolledBack,
    Conflict,
    Altered,
    Retired,
    Incomplete,
    /** @brief The only verdict that may be called verified, and only together with the range it covers. */
    Anchored,
    /** @brief "Internally consistent, unanchored": the provider holds nothing for the stream. */
    Unanchored,
    /** @brief The same coverage as Unanchored, but the provider did not answer. */
    AnchorUnavailable
};

[[nodiscard]] constexpr std::string_view verdictName(Verdict verdict) noexcept {
    switch (verdict) {
        case Verdict::Inconsistent:
            return "inconsistent";
        case Verdict::CannotVerify:
            return "cannot verify";
        case Verdict::RolledBack:
            return "rolled back";
        case Verdict::Conflict:
            return "conflict";
        case Verdict::Altered:
            return "altered";
        case Verdict::Retired:
            return "retired";
        case Verdict::Incomplete:
            return "incomplete";
        case Verdict::Anchored:
            return "anchored";
        case Verdict::Unanchored:
            return "internally consistent, unanchored";
        case Verdict::AnchorUnavailable:
            return "internally consistent, anchor unavailable";
    }
    return "";
}

/** @brief What the verdict rests on. The report names the cause, as 7.5 requires. */
enum class VerdictCause : std::uint8_t {
    None,
    /** @brief The stream identity is one AuditEvent rejects: a caller fault, not a finding about the records. */
    InvalidStreamIdentity,
    /** @brief The stream's version is not implemented here: nothing was decoded, so no digest can be recomputed. */
    UnsupportedStreamVersion,
    /** @brief A segment's layout version is unknown to this reader: nothing was read from it, and the stream is named Cannot verify (9.4). */
    UnsupportedLayoutVersion,
    /** @brief A segment or the medium could not be read, so the records held may not be the stream (9.6). */
    StorageUnreadable,
    /** @brief The segment headers and the records disagree: a duplicate or missing segment index, or a header whose firstSequence is not the next record (9.5).
     */
    SegmentLayoutInconsistent,
    /** @brief The provider holds an anchor whose anchorFormat or canonicalVersion this verifier does not know. */
    AnchorFormatUnknown,
    /** @brief The provider holds an anchor with a mandatory field missing. */
    AnchorMissingField,
    /** @brief A trim removed records past the anchor's position, so H_p cannot be recomputed (10.4). */
    TrimPastAnchor,
    ProviderHeadBelowRetained,
    StreamAnchorBelowRetained,
    RetirementUndone,
    /** @brief A retained stream has neither an anchor nor a retirement at the provider. */
    StreamMissing,
    AnchorConflict,
    /** @brief The recomputed H_p differs from the anchor's digest. */
    AnchorDigestMismatch,
    /** @brief The recomputed digest at the retained checkpoint differs from the retained one. */
    RetainedCheckpointMismatch,
    /** @brief The last record present is below the anchor's position: records m+1 … p are missing. */
    LogEndsBeforeAnchor,
    /** @brief The last record present is below the retained checkpoint. */
    LogEndsBeforeRetained,
    /** @brief The provider holds no anchor and no retirement for the stream. */
    NoAnchor,
    ProviderUnavailable
};

/** @brief Outcome of the retained-position check of 7.4. */
enum class RetainedOutcome : std::uint8_t {
    /** @brief Nothing retained for this stream or provider: "rollback not excluded: no retained position". */
    NoneRetained,
    Passed,
    RolledBack,
    /** @brief The provider did not answer, so the position checks could not run. */
    NotChecked,
    /** @brief A trim went past the retained checkpoint, which ends the protection it gave its prefix (10.4). */
    CheckpointTrimmed
};

/** @brief Age staleness of the anchor against the declared bound T (7.3). It never reduces coverage. */
enum class AgeStatus : std::uint8_t {
    /** @brief No bound T was declared. */
    NotChecked,
    Fresh,
    Stale,
    /** @brief The anchor's acceptedTime or the verification time is unavailable. */
    Unknown
};

/** @brief Verifier configuration declared by the integrator (7.3), never a constant of the library. */
struct VerifierConfig {
    /** @brief The age bound T; empty when the integrator declared none. */
    std::optional<std::chrono::nanoseconds> maxAnchorAge;
    /** @brief The verification time, host-supplied. */
    core::RawTime verificationTime = core::RawTime::unavailable();
};

/** @brief The highest anchor verified for one stream (7.4). */
struct RetainedAnchor {
    std::uint64_t position = 0;
    Sha256Digest  digest{};
    std::uint64_t counter = 0;
    bool          retired = false;

    [[nodiscard]] bool operator==(const RetainedAnchor&) const noexcept = default;
};

/**
 * @brief The reader's retained position (7.4): the highest head seen per provider, and the highest verified anchor per stream.
 *
 * Two separate values, never compared with each other. Values only rise. Where this is stored is the reader's decision: it must
 * live off the device and outside the log writer's authority, so the class holds values and leaves persistence to its owner.
 */
class RetainedPosition {
public:
    [[nodiscard]] bool empty() const noexcept {
        return heads.empty() && anchors.empty();
    }
    [[nodiscard]] std::optional<std::uint64_t> head(std::string_view providerId) const {
        const auto found = heads.find(std::string{providerId});
        return found == heads.end() ? std::nullopt : std::optional<std::uint64_t>{found->second};
    }
    [[nodiscard]] std::optional<RetainedAnchor> anchor(std::string_view streamId) const {
        const auto found = anchors.find(std::string{streamId});
        return found == anchors.end() ? std::nullopt : std::optional<RetainedAnchor>{found->second};
    }

    /** @brief Raise a provider's retained head. A lower value changes nothing. */
    void raiseHead(std::string_view providerId, std::uint64_t value) {
        auto& slot = heads[std::string{providerId}];
        slot       = std::max(slot, value);
    }
    /**
     * @brief Raise a stream's retained anchor. It is replaced only by a higher position. At the same position and digest the
     * counter and the retired flag only rise. Anything else changes nothing, so a lower or diverging value is never accepted.
     */
    void raiseAnchor(std::string_view streamId, const RetainedAnchor& value) {
        const auto [slot, inserted] = anchors.try_emplace(std::string{streamId}, value);
        if (inserted)
            return;
        RetainedAnchor& held = slot->second;
        if (value.position > held.position) {
            held = value;
        } else if (value.position == held.position && value.digest == held.digest) {
            held.counter = std::max(held.counter, value.counter);
            held.retired = held.retired || value.retired;
        }
    }

    /** @brief Every retained entry, for the owner to persist or restore. */
    [[nodiscard]] const std::map<std::string, std::uint64_t>& allHeads() const noexcept {
        return heads;
    }
    [[nodiscard]] const std::map<std::string, RetainedAnchor>& allAnchors() const noexcept {
        return anchors;
    }

private:
    std::map<std::string, std::uint64_t>  heads;
    std::map<std::string, RetainedAnchor> anchors;
};

/**
 * @brief The report of 7.5 for one stream instance. It is never folded with another instance's.
 *
 * Coverage: records `firstRetained … anchoredThrough` are matched against the anchor, `unanchoredFrom … lastPresent` are
 * internally consistent only. Nothing is "verified" unless verdict is Anchored, and then anchoredThrough carries the range.
 */
struct StreamReport {
    std::string  streamId;
    Verdict      verdict = Verdict::CannotVerify;
    VerdictCause cause   = VerdictCause::None;
    /** @brief For Inconsistent, the first finding of the chain check; Ok otherwise. */
    ChainFinding chainFinding = ChainFinding::Ok;
    /** @brief For Inconsistent, the expected sequence of the first failing record. */
    std::optional<std::uint64_t> failedAt;
    /** @brief For Cannot verify on an unknown layout, the version that was found (9.4). */
    std::optional<std::uint16_t> unknownLayoutVersion;

    /** @brief s: the first record retained, 1 or q + 1 after a trim. */
    std::uint64_t firstRetained = 1;
    /** @brief m: the last record that checked out internally; firstRetained - 1 when none. Meaningless for CannotVerify on an unknown version. */
    std::uint64_t lastPresent = 0;
    /** @brief p, set only for Anchored: `firstRetained … anchoredThrough` matches the anchor's digest. */
    std::optional<std::uint64_t> anchoredThrough;
    /** @brief p + 1, set when records exist past the anchor: they are internally consistent and unanchored (stale by position). */
    std::optional<std::uint64_t> unanchoredFrom;

    /** @brief The anchor, or the retirement's final anchor, the report relied on: providerId, counter, canonicalVersion, acceptedTime. */
    std::optional<Anchor> anchor;
    AgeStatus             age      = AgeStatus::NotChecked;
    RetainedOutcome       retained = RetainedOutcome::NoneRetained;

    /** @brief True when an Anchored verdict was reached with no retained position: a joint rollback of log and anchor is not excluded. */
    [[nodiscard]] bool rollbackNotExcluded() const noexcept {
        return verdict == Verdict::Anchored && retained == RetainedOutcome::NoneRetained;
    }
    /** @brief Records past the anchor are present: the anchor is stale by position and coverage ends at its position. */
    [[nodiscard]] bool staleByPosition() const noexcept {
        return unanchoredFrom.has_value();
    }
};

/**
 * @brief Verifies stored records against a provider's anchors and the reader's retained position.
 *
 * The chain check of AuditChainVerifier runs first and stays authoritative: an Inconsistent finding is reported before anything
 * the provider says. Then the verdicts of 7.5 apply in their listed order. Only a clean Anchored or Retired result raises the
 * retained position (7.4), so a rollback that was reported cannot be accepted silently afterwards.
 *
 * With one contract version known, an anchor naming another canonicalVersion is unusable (CannotVerify) before any comparison, so
 * the Altered-by-version case of 7.5 cannot arise here; it needs a second known version.
 */
class AnchorVerifier {
public:
    /** @brief Holds references: the provider and the retained position must outlive the verifier. */
    AnchorVerifier(AnchorProvider& anchorProvider, RetainedPosition& retainedPosition, VerifierConfig verifierConfig = {}) noexcept
        : provider(&anchorProvider), retained(&retainedPosition), config(verifierConfig) {}

    /** @brief Verify one stream instance whose records are given in storage order. An empty span means the log holds none. */
    [[nodiscard]] StreamReport
    verify(std::string_view streamId, std::span<const StoredRecord> records, const StreamStart& start = {}, const StoredLayout& layout = {}) {
        StreamReport report;
        report.streamId      = std::string{streamId};
        report.firstRetained = start.afterSequence + 1;
        report.lastPresent   = start.afterSequence;

        // 1. The chain: Inconsistent comes before anything the provider says.
        AuditChainVerifier chain{streamId, start.afterSequence, start.afterDigest};
        if (!chain.valid())
            return finish(report, Verdict::CannotVerify, VerdictCause::InvalidStreamIdentity);

        const LatestAnswer  latest  = provider->latest(streamId);
        const StreamsAnswer listing = provider->streams();
        const auto          before  = retained->anchor(streamId);
        listedHead.reset();
        if (const auto* all = std::get_if<ProviderListing>(&listing))
            listedHead = std::pair{all->providerId, all->head};

        const std::optional<Anchor> held       = anchorOf(latest);
        const Retirement*           retirement = std::get_if<Retirement>(&latest);

        // A retirement's final anchor stands in for the anchor (7.5), so its digest is captured while the records are read.
        listedRetired                = retirement != nullptr;
        std::uint64_t anchorPosition = 0;
        if (held.has_value())
            anchorPosition = held->position;
        else if (retirement != nullptr)
            anchorPosition = retirement->finalAnchor.position;
        const std::uint64_t checkpoint = before.has_value() ? before->position : 0;

        std::optional<Sha256Digest> digestAtAnchor;
        std::optional<Sha256Digest> digestAtCheckpoint;
        const auto                  noteStart = [&](std::uint64_t position, std::optional<Sha256Digest>& slot) {
            if (position != 0 && position == start.afterSequence)
                slot = start.afterDigest;
        };
        noteStart(anchorPosition, digestAtAnchor);
        noteStart(checkpoint, digestAtCheckpoint);

        std::size_t recordCount = 0;
        for (const StoredRecord& record : records) {
            ++recordCount;
            const ChainFinding finding = chain.check(record.bytes, record.digest);
            if (finding == ChainFinding::Ok) {
                const std::uint64_t through = chain.verifiedThrough();
                if (anchorPosition != 0 && through == anchorPosition)
                    digestAtAnchor = chain.headDigest();
                if (checkpoint != 0 && through == checkpoint)
                    digestAtCheckpoint = chain.headDigest();
            } else if (finding != ChainFinding::UnsupportedVersion) {
                report.chainFinding = finding;
                break;
            }
        }
        report.lastPresent = chain.verifiedThrough();
        if (const auto failed = chain.failedAt(); failed.has_value()) {
            report.failedAt = failed;
            return finish(report, Verdict::Inconsistent, VerdictCause::None);
        }
        // Headers and records must agree (9.5). It comes before Cannot verify, as Inconsistent does in 7.5.
        if (const auto broken = firstBoundaryBreak(layout, start); broken.has_value()) {
            report.failedAt = broken;
            return finish(report, Verdict::Inconsistent, VerdictCause::SegmentLayoutInconsistent);
        }
        if (layout.unknownLayoutVersion.has_value()) {
            report.unknownLayoutVersion = layout.unknownLayoutVersion;
            return finish(report, Verdict::CannotVerify, VerdictCause::UnsupportedLayoutVersion);
        }
        if (chain.cannotVerify())
            return finish(report, Verdict::CannotVerify, VerdictCause::UnsupportedStreamVersion);
        if (layout.unreadable)
            return finish(report, Verdict::CannotVerify, VerdictCause::StorageUnreadable);
        const std::uint64_t present = report.lastPresent;

        // The anchor the verdicts below rest on: the latest anchor, or a retirement's final anchor.
        const bool                  retiredStream = retirement != nullptr;
        const std::optional<Anchor> effective     = retiredStream ? std::optional<Anchor>{retirement->finalAnchor} : held;
        report.anchor                             = effective;

        // 2. An anchor the provider holds but this verifier cannot use is never absence and never a match.
        if (effective.has_value() && !effective->usable()) {
            const VerdictCause cause = !effective->hasMandatoryFields() ? VerdictCause::AnchorMissingField : VerdictCause::AnchorFormatUnknown;
            return finish(report, Verdict::CannotVerify, cause);
        }
        const bool recordsPresent = recordCount != 0;
        if (effective.has_value() && start.afterSequence > effective->position && (recordsPresent || !retiredStream))
            return finish(report, Verdict::CannotVerify, VerdictCause::TrimPastAnchor);

        // 3. Rolled back: a retained-position check fails (7.4).
        report.retained = retainedOutcome(before, latest, listing, retirement, held);
        if (const auto cause = rollbackCause(before, latest, listing, retirement, held); cause != VerdictCause::None)
            return finish(report, Verdict::RolledBack, cause);

        // 4. Conflict: the provider's two answers disagree about the stream (7.1).
        if (effective.has_value() && conflictsWithListing(*effective, listing))
            return finish(report, Verdict::Conflict, VerdictCause::AnchorConflict);

        // The retained checkpoint is re-verified whatever position the current anchor has, unless a trim went past it (10.4).
        const bool checkpointApplies = before.has_value() && (!retiredStream || recordsPresent) && start.afterSequence <= checkpoint;
        if (before.has_value() && start.afterSequence > checkpoint && report.retained == RetainedOutcome::Passed)
            report.retained = RetainedOutcome::CheckpointTrimmed;

        // 5. Altered: a recomputed digest differs from the one that was anchored or retained.
        if (effective.has_value() && effective->position <= present && digestAtAnchor.has_value() && *digestAtAnchor != effective->digest)
            return finish(report, Verdict::Altered, VerdictCause::AnchorDigestMismatch);
        if (checkpointApplies && checkpoint <= present && digestAtCheckpoint.has_value() && *digestAtCheckpoint != before->digest)
            return finish(report, Verdict::Altered, VerdictCause::RetainedCheckpointMismatch);

        // 6. Retired: the stream aged out under retention and the log holds none of it. Not a finding.
        if (retiredStream && !recordsPresent)
            return finish(report, Verdict::Retired, VerdictCause::None);

        // 7. Incomplete: records the anchor or the retained checkpoint covers are missing.
        if (effective.has_value() && present < effective->position)
            return finish(report, Verdict::Incomplete, VerdictCause::LogEndsBeforeAnchor);
        if (checkpointApplies && present < checkpoint)
            return finish(report, Verdict::Incomplete, VerdictCause::LogEndsBeforeRetained);

        // 8. Anchored: H_p matches. The range it covers is stated, and what lies past it is unanchored.
        if (effective.has_value()) {
            report.anchoredThrough = effective->position;
            if (present > effective->position)
                report.unanchoredFrom = effective->position + 1;
            report.age = ageOf(*effective);
            return finish(report, Verdict::Anchored, VerdictCause::None);
        }

        // 9. Unanchored, or the provider did not answer: the same coverage, a different cause.
        if (std::holds_alternative<ProviderUnavailable>(latest))
            return finish(report, Verdict::AnchorUnavailable, VerdictCause::ProviderUnavailable);
        return finish(report, Verdict::Unanchored, VerdictCause::NoAnchor);
    }

    /**
     * @brief Report every stream the provider or the retained position knows that the log holds no record of.
     *
     * An anchored stream the log lacks is Incomplete with m = 0, a retired one is Retired, and a retained stream the provider
     * dropped without a retirement is RolledBack. Pass the stream identities the log does hold.
     */
    [[nodiscard]] std::vector<StreamReport> verifyUnlisted(std::span<const std::string> logStreams) {
        std::set<std::string> known;
        if (const auto listing = provider->streams(); const auto* all = std::get_if<ProviderListing>(&listing)) {
            for (const StreamEntry& entry : all->entries)
                known.insert(entryAnchor(entry).streamId);
        }
        for (const auto& [id, value] : retained->allAnchors())
            known.insert(id);
        for (const std::string& id : logStreams)
            known.erase(id);
        std::vector<StreamReport> reports;
        reports.reserve(known.size());
        for (const std::string& id : known)
            reports.push_back(verify(id, {}));
        return reports;
    }

private:
    /** @brief The expected sequence of the first record that does not follow, when a header or a segment index breaks the continuity (9.5). */
    [[nodiscard]] static std::optional<std::uint64_t> firstBoundaryBreak(const StoredLayout& layout, const StreamStart& start) noexcept {
        std::uint64_t                expected = start.afterSequence + 1;
        std::optional<std::uint32_t> previous;
        for (const SegmentBoundary& segment : layout.segments) {
            if ((previous.has_value() && segment.segmentIndex != *previous + 1) || segment.firstSequence != expected)
                return expected;
            previous  = segment.segmentIndex;
            expected += segment.recordCount;
        }
        return std::nullopt;
    }

    [[nodiscard]] static const Anchor& entryAnchor(const StreamEntry& entry) noexcept {
        if (const auto* anchor = std::get_if<Anchor>(&entry))
            return *anchor;
        return std::get_if<Retirement>(&entry)->finalAnchor;
    }
    [[nodiscard]] static std::optional<Anchor> anchorOf(const LatestAnswer& answer) {
        if (const auto* anchor = std::get_if<Anchor>(&answer))
            return *anchor;
        return std::nullopt;
    }

    /** @brief Position or counter below the retained anchor, or the same position with another digest (7.4, Stream anchor). */
    [[nodiscard]] static bool belowRetained(const Anchor& current, const RetainedAnchor& kept) noexcept {
        return current.counter < kept.counter || current.position < kept.position || (current.position == kept.position && current.digest != kept.digest);
    }

    [[nodiscard]] VerdictCause rollbackCause(const std::optional<RetainedAnchor>& before,
                                             const LatestAnswer&                  latest,
                                             const StreamsAnswer&                 listing,
                                             const Retirement*                    retirement,
                                             const std::optional<Anchor>&         held) const {
        if (const auto* all = std::get_if<ProviderListing>(&listing)) {
            if (const auto kept = retained->head(all->providerId); kept.has_value() && all->head < *kept)
                return VerdictCause::ProviderHeadBelowRetained;
        }
        if (!before.has_value() || std::holds_alternative<ProviderUnavailable>(latest))
            return VerdictCause::None;
        if (std::holds_alternative<AnchorAbsent>(latest))
            return VerdictCause::StreamMissing;
        if (held.has_value() && before->retired)
            return VerdictCause::RetirementUndone;
        if (held.has_value())
            return belowRetained(*held, *before) ? VerdictCause::StreamAnchorBelowRetained : VerdictCause::None;
        return belowRetained(retirement->finalAnchor, *before) ? VerdictCause::StreamAnchorBelowRetained : VerdictCause::None;
    }

    [[nodiscard]] RetainedOutcome retainedOutcome(const std::optional<RetainedAnchor>& before,
                                                  const LatestAnswer&                  latest,
                                                  const StreamsAnswer&                 listing,
                                                  const Retirement*                    retirement,
                                                  const std::optional<Anchor>&         held) const {
        if (rollbackCause(before, latest, listing, retirement, held) != VerdictCause::None)
            return RetainedOutcome::RolledBack;
        const auto* all = std::get_if<ProviderListing>(&listing);
        if (all == nullptr || std::holds_alternative<ProviderUnavailable>(latest))
            return RetainedOutcome::NotChecked;
        return before.has_value() || retained->head(all->providerId).has_value() ? RetainedOutcome::Passed : RetainedOutcome::NoneRetained;
    }

    /** @brief Two anchors of one provider and stream conflict: the same position with different digests, a position that falls while the counter rises,
     * or one counter shared by two positions (7.1). */
    [[nodiscard]] static bool conflictsWithListing(const Anchor& current, const StreamsAnswer& listing) {
        const auto* all = std::get_if<ProviderListing>(&listing);
        if (all == nullptr)
            return false;
        return std::ranges::any_of(all->entries, [&](const StreamEntry& entry) {
            const Anchor& other = entryAnchor(entry);
            if (other.streamId != current.streamId || other.providerId != current.providerId)
                return false;
            if (other.position == current.position)
                return other.digest != current.digest;
            if (other.counter == current.counter)
                return true;
            return (other.position < current.position) == (other.counter > current.counter);
        });
    }

    [[nodiscard]] AgeStatus ageOf(const Anchor& anchor) const noexcept {
        if (!config.maxAnchorAge.has_value())
            return AgeStatus::NotChecked;
        if (anchor.acceptedTime.availability() != core::TimeAvailability::Available
            || config.verificationTime.availability() != core::TimeAvailability::Available)
            return AgeStatus::Unknown;
        const auto age = config.verificationTime.value() - anchor.acceptedTime.value();
        return age > *config.maxAnchorAge ? AgeStatus::Stale : AgeStatus::Fresh;
    }

    /** @brief Close a report. A clean Anchored or Retired result is the only one that raises the retained position (7.4). */
    [[nodiscard]] StreamReport finish(StreamReport& report, Verdict verdict, VerdictCause cause) {
        report.verdict = verdict;
        report.cause   = cause;
        if (verdict == Verdict::Anchored || verdict == Verdict::Retired) {
            if (listedHead.has_value())
                retained->raiseHead(listedHead->first, listedHead->second);
            if (report.anchor.has_value())
                retained->raiseAnchor(report.streamId,
                                      RetainedAnchor{.position = report.anchor->position,
                                                     .digest   = report.anchor->digest,
                                                     .counter  = report.anchor->counter,
                                                     .retired  = listedRetired});
        }
        return report;
    }

    AnchorProvider*   provider;
    RetainedPosition* retained;
    VerifierConfig    config;
    /** @brief The provider head seen by the verification in progress, raised into the retained position on a clean result. */
    std::optional<std::pair<std::string, std::uint64_t>> listedHead;
    /** @brief Whether latest() returned a retirement in the verification in progress, whatever became of the records. */
    bool listedRetired = false;
};

}  // namespace mddlog::adapter
