/** @brief Anchor claims, the anchor provider interface and an in-memory test provider (ADR-004 Decision 7.1 and 7.2). Adapter zone only. */

export module mddlog.adapter.auditanchor;

import std;
import mddlog.core.auditevent;
export import mddlog.adapter.sha256;
export import mddlog.adapter.auditcanonical;

export namespace mddlog::adapter {

/** @brief The anchor layout this library writes and reads (7.1). Zero is never valid. */
inline constexpr std::uint16_t anchorFormatVersion = 1;

/**
 * @brief What the log contained at `position`, as the adapter computed it while writing (7.3).
 *
 * The provider validates the claim, it never sees the records. Build it with makeAnchorClaim().
 */
struct AnchorClaim {
    std::uint16_t anchorFormat     = anchorFormatVersion;
    std::uint16_t canonicalVersion = canonicalContractVersion;
    std::string   streamId;
    std::uint64_t position = 0;
    Sha256Digest  digest{};
};

[[nodiscard]] inline AnchorClaim makeAnchorClaim(std::string_view streamId, std::uint64_t position, const Sha256Digest& digest) {
    return AnchorClaim{.streamId = std::string{streamId}, .position = position, .digest = digest};
}

/** @brief Where and when the provider accepted a claim (7.1, provider stamp). */
struct AnchorStamp {
    std::string   providerId;
    std::uint64_t counter      = 0;
    core::RawTime acceptedTime = core::RawTime::unavailable();
};

/** @brief An accepted claim and its stamp. Every field is mandatory (7.1). */
struct Anchor {
    std::uint16_t anchorFormat     = 0;
    std::uint16_t canonicalVersion = 0;
    std::string   streamId;
    std::uint64_t position = 0;
    Sha256Digest  digest{};
    std::string   providerId;
    std::uint64_t counter      = 0;
    core::RawTime acceptedTime = core::RawTime::unavailable();

    /** @brief A mandatory field is missing: the anchor is unusable whatever versions a verifier knows (7.1). */
    [[nodiscard]] bool hasMandatoryFields() const noexcept {
        return anchorFormat != 0 && canonicalVersion != 0 && !streamId.empty() && position >= 1 && !providerId.empty() && counter >= 1;
    }
    /** @brief Usable: complete and in a layout and contract version this library knows (7.1). */
    [[nodiscard]] bool usable() const noexcept {
        return hasMandatoryFields() && anchorFormat == anchorFormatVersion && canonicalVersion == canonicalContractVersion;
    }
};

/** @brief A stream's final anchor plus the retirement's own counter and time (7.2). Kept for the provider's lifetime. */
struct Retirement {
    Anchor        finalAnchor;
    std::uint64_t counter     = 0;
    core::RawTime retiredTime = core::RawTime::unavailable();
};

/** @brief The provider could not be reached or written to. Nothing was accepted. */
struct ProviderUnavailable {
    [[nodiscard]] bool operator==(const ProviderUnavailable&) const noexcept = default;
};

/** @brief The provider holds neither an anchor nor a retirement for the stream (7.3, Absence). */
struct AnchorAbsent {
    [[nodiscard]] bool operator==(const AnchorAbsent&) const noexcept = default;
};

enum class AdvanceRefusal : std::uint8_t {
    /** @brief The position is not above the last accepted one for that stream. */
    PositionNotIncreasing,
    /** @brief Same position with a different digest, or the stream is retired and the claim does not fit its final anchor. */
    Conflict,
    /** @brief A mandatory field of the claim is missing or invalid. */
    Malformed
};

enum class RetireRefusal : std::uint8_t {
    /** @brief The position is not the stream's highest accepted position. */
    Conflict,
    UnknownStream
};

/** @brief Answer to advance(): accepted with a stamp, refused with a reason, or unavailable (7.2). */
using AdvanceAnswer = std::variant<AnchorStamp, AdvanceRefusal, ProviderUnavailable>;
/** @brief Answer to retire(): the retirement's own stamp, a refusal, or unavailable (7.2). */
using RetireAnswer = std::variant<AnchorStamp, RetireRefusal, ProviderUnavailable>;
/** @brief Answer to latest(): the highest accepted anchor, the retirement, absent, or unavailable (7.2). */
using LatestAnswer = std::variant<Anchor, Retirement, AnchorAbsent, ProviderUnavailable>;

/** @brief One stream the provider holds: its anchor or, once aged out, its retirement. */
using StreamEntry = std::variant<Anchor, Retirement>;

/** @brief The provider's head and every stream it holds an entry for (7.2, streams()). */
struct ProviderListing {
    std::string providerId;
    /** @brief Highest counter ever assigned. Kept as a value of its own, so it never decreases, even after a retirement. */
    std::uint64_t            head = 0;
    std::vector<StreamEntry> entries;
};

using StreamsAnswer = std::variant<ProviderListing, ProviderUnavailable>;

/**
 * @brief The four operations of 7.2. An interface contract only: eligibility (7.2, conditions 1 to 5) is a property of an
 * implementation and of its deployment, which this interface cannot establish.
 */
class AnchorProvider {
public:
    AnchorProvider()                                 = default;
    AnchorProvider(const AnchorProvider&)            = delete;
    AnchorProvider& operator=(const AnchorProvider&) = delete;
    AnchorProvider(AnchorProvider&&)                 = delete;
    AnchorProvider& operator=(AnchorProvider&&)      = delete;
    virtual ~AnchorProvider()                        = default;

    [[nodiscard]] virtual AdvanceAnswer advance(const AnchorClaim& claim)                         = 0;
    [[nodiscard]] virtual RetireAnswer  retire(std::string_view streamId, std::uint64_t position) = 0;
    [[nodiscard]] virtual LatestAnswer  latest(std::string_view streamId)                         = 0;
    [[nodiscard]] virtual StreamsAnswer streams()                                                 = 0;
};

/**
 * @brief In-memory provider for tests. It enforces monotonic acceptance and keeps retirements like an eligible provider (7.2),
 * but it holds its state in process memory next to its caller, so it fails custody (condition 1) and is not an anchor to rely on.
 *
 * The fault-injection members below deliberately break eligibility so that a verifier can be shown each finding.
 */
class InMemoryAnchorProvider final : public AnchorProvider {
public:
    /** @brief Everything the provider holds, copyable so a test can restore an earlier state (a rollback). */
    struct State {
        std::uint64_t                     head = 0;
        std::map<std::string, Anchor>     anchors;
        std::map<std::string, Retirement> retirements;
    };

    explicit InMemoryAnchorProvider(std::string identity) : id(std::move(identity)) {}

    [[nodiscard]] AdvanceAnswer advance(const AnchorClaim& claim) override {
        if (!available)
            return ProviderUnavailable{};
        if (claim.anchorFormat == 0 || claim.canonicalVersion == 0 || claim.position == 0 || !core::AuditEvent::validStreamId(claim.streamId))
            return AdvanceRefusal::Malformed;
        if (const auto retired = state.retirements.find(claim.streamId); retired != state.retirements.end()) {
            const Anchor& last = retired->second.finalAnchor;
            return claim.position <= last.position ? AdvanceRefusal::PositionNotIncreasing : AdvanceRefusal::Conflict;
        }
        if (const auto held = state.anchors.find(claim.streamId); held != state.anchors.end()) {
            if (claim.position == held->second.position && claim.digest != held->second.digest)
                return AdvanceRefusal::Conflict;
            if (claim.position <= held->second.position)
                return AdvanceRefusal::PositionNotIncreasing;
        }
        AnchorStamp stamp{.providerId = id, .counter = ++state.head, .acceptedTime = now};
        state.anchors.insert_or_assign(claim.streamId,
                                       Anchor{.anchorFormat     = claim.anchorFormat,
                                              .canonicalVersion = claim.canonicalVersion,
                                              .streamId         = claim.streamId,
                                              .position         = claim.position,
                                              .digest           = claim.digest,
                                              .providerId       = stamp.providerId,
                                              .counter          = stamp.counter,
                                              .acceptedTime     = stamp.acceptedTime});
        return stamp;
    }

    [[nodiscard]] RetireAnswer retire(std::string_view streamId, std::uint64_t position) override {
        if (!available)
            return ProviderUnavailable{};
        const std::string key{streamId};
        const auto        held = state.anchors.find(key);
        if (held == state.anchors.end())
            return state.retirements.contains(key) ? RetireRefusal::Conflict : RetireRefusal::UnknownStream;
        if (held->second.position != position)
            return RetireRefusal::Conflict;
        AnchorStamp stamp{.providerId = id, .counter = ++state.head, .acceptedTime = now};
        state.retirements.insert_or_assign(key, Retirement{.finalAnchor = held->second, .counter = stamp.counter, .retiredTime = now});
        state.anchors.erase(held);
        return stamp;
    }

    [[nodiscard]] LatestAnswer latest(std::string_view streamId) override {
        if (!available)
            return ProviderUnavailable{};
        const std::string key{streamId};
        if (const auto retired = state.retirements.find(key); retired != state.retirements.end())
            return retired->second;
        if (const auto held = state.anchors.find(key); held != state.anchors.end())
            return held->second;
        return AnchorAbsent{};
    }

    [[nodiscard]] StreamsAnswer streams() override {
        if (!available)
            return ProviderUnavailable{};
        ProviderListing listing{.providerId = id, .head = state.head, .entries = {}};
        for (const auto& [key, anchor] : state.anchors)
            listing.entries.emplace_back(anchor);
        for (const auto& [key, retirement] : state.retirements)
            listing.entries.emplace_back(retirement);
        return listing;
    }

    // --- Fault injection: each of these breaks an eligibility condition on purpose. ---

    /** @brief Answer every operation as unavailable until set back. */
    void setAvailable(bool value) noexcept {
        available = value;
    }
    /** @brief The provider's clock, stamped on later acceptances. Unavailable by default. */
    void setClock(core::RawTime time) noexcept {
        now = time;
    }
    [[nodiscard]] State snapshot() const {
        return state;
    }
    /** @brief Return to an earlier state, head included: what an adversary restoring the anchor medium with the log does. */
    void restore(State earlier) {
        state = std::move(earlier);
    }
    /** @brief Store an anchor exactly as given, bypassing every check. Raises the head to its counter if above. */
    void inject(const Anchor& anchor) {
        state.anchors.insert_or_assign(anchor.streamId, anchor);
        state.head = std::max(state.head, anchor.counter);
    }
    /** @brief Forget a stream's anchor and retirement, as a deletion by someone who can reach the medium. */
    void erase(std::string_view streamId) {
        const std::string key{streamId};
        state.anchors.erase(key);
        state.retirements.erase(key);
    }

private:
    std::string   id;
    State         state;
    bool          available = true;
    core::RawTime now       = core::RawTime::unavailable();
};

}  // namespace mddlog::adapter
