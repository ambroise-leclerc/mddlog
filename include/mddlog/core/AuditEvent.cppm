/** @brief Bounded runtime audit event and explicit admission result. */

export module mddlog.core.auditevent;

import std;
export import mddlog.core.inlinestring;
export import mddlog.core.record;

export namespace mddlog::core {

inline constexpr std::size_t auditActionCapacity      = 64;
inline constexpr std::size_t auditActorCapacity       = 64;
inline constexpr std::size_t auditTargetCapacity      = 96;
inline constexpr std::size_t auditReferenceCapacity   = 64;
inline constexpr std::size_t auditCorrelationCapacity = 96;
inline constexpr std::size_t auditStreamCapacity      = 96;
inline constexpr std::size_t auditDetailCapacity      = 160;

/** @brief Runtime event domain; this set remains subject to ADR-002 review. */
enum class AuditCategory : std::uint8_t { Lifecycle, Configuration, Access, RiskControl, Operator };
/** @brief A request, an optional confirmation, or the actual outcome. */
enum class AuditPhase : std::uint8_t { Requested, Confirmed, Executed, Failed };
enum class AuditField : std::uint8_t { Action, Actor, Target, RequirementRef, RiskRef, CorrelationId, StreamId };
enum class AuditRefusalReason : std::uint8_t { InvalidIdentifier, RingFull, SequenceExhausted, InvalidStream };

struct AuditRefusal {
    AuditRefusalReason reason = AuditRefusalReason::RingFull;
    AuditField         field  = AuditField::Action;
};

/** @brief Synchronous, in-memory admission only; no hand-off or durability is implied. */
class [[nodiscard]] AuditWriteResult {
public:
    [[nodiscard]] static constexpr AuditWriteResult admitted(std::uint64_t assignedSequence, bool shortened) noexcept {
        return {std::nullopt, assignedSequence, shortened};
    }
    [[nodiscard]] static constexpr AuditWriteResult refused(AuditRefusal failure) noexcept {
        return {failure, 0, false};
    }
    [[nodiscard]] constexpr bool wasAdmitted() const noexcept {
        return !failureValue.has_value();
    }
    [[nodiscard]] constexpr std::optional<AuditRefusal> refusal() const noexcept {
        return failureValue;
    }
    [[nodiscard]] constexpr std::uint64_t sequence() const noexcept {
        return sequenceValue;
    }
    [[nodiscard]] constexpr bool detailTruncated() const noexcept {
        return shortenedValue;
    }

private:
    constexpr AuditWriteResult(std::optional<AuditRefusal> failure, std::uint64_t assignedSequence, bool shortened) noexcept
        : failureValue(failure), sequenceValue(assignedSequence), shortenedValue(shortened) {}
    std::optional<AuditRefusal> failureValue;
    std::uint64_t               sequenceValue;
    bool                        shortenedValue;
};

/** @brief Caller-supplied facts; the audit ring assigns stream identity and sequence. */
struct AuditInput {
    AuditCategory                category = AuditCategory::Lifecycle;
    AuditPhase                   phase    = AuditPhase::Requested;
    RawTime                      time     = RawTime::unavailable();
    std::string_view             action;
    std::string_view             actor;
    std::string_view             target;
    std::string_view             requirementRef;
    std::string_view             riskRef;
    std::string_view             correlationId;
    std::optional<std::uint64_t> sourceSequence;
    std::string_view             detail;
};

/**
 * @brief Owned audit record with exact identifiers and a truncatable UTF-8 detail.
 *
 * Identifier bytes are printable ASCII letters, digits, underscore, dot, colon, slash or
 * hyphen. Action, target and stream identity must be nonempty. Optional identifiers may be empty.
 * The caller owns uniqueness of stream identities across producers and boot sessions.
 */
class AuditEvent {
public:
    constexpr AuditEvent() noexcept = default;

    [[nodiscard]] constexpr AuditWriteResult assign(const AuditInput& input, std::string_view streamId, std::uint64_t sequence) noexcept {
        if (sequence == 0)
            return AuditWriteResult::refused({AuditRefusalReason::SequenceExhausted});
        if (auto failure = validate(input, streamId); failure.has_value())
            return AuditWriteResult::refused(*failure);
        AuditEvent next;
        (void)next.actionValue.assignExact(input.action);
        (void)next.actorValue.assignExact(input.actor);
        (void)next.targetValue.assignExact(input.target);
        (void)next.requirementValue.assignExact(input.requirementRef);
        (void)next.riskValue.assignExact(input.riskRef);
        (void)next.correlationValue.assignExact(input.correlationId);
        (void)next.streamValue.assignExact(streamId);
        next.categoryValue       = input.category;
        next.phaseValue          = input.phase;
        next.timeValue           = input.time;
        next.sequenceValue       = sequence;
        next.sourceSequenceValue = input.sourceSequence;
        next.shortenedValue      = next.detailValue.assignTruncating(input.detail);
        *this                    = next;
        return AuditWriteResult::admitted(sequence, shortenedValue);
    }

    [[nodiscard]] static constexpr std::optional<AuditRefusal> validate(const AuditInput& input, std::string_view streamId) noexcept {
        if (!validIdentifier(input.action, auditActionCapacity, true))
            return AuditRefusal{AuditRefusalReason::InvalidIdentifier, AuditField::Action};
        if (!validIdentifier(input.actor, auditActorCapacity, false))
            return AuditRefusal{AuditRefusalReason::InvalidIdentifier, AuditField::Actor};
        if (!validIdentifier(input.target, auditTargetCapacity, true))
            return AuditRefusal{AuditRefusalReason::InvalidIdentifier, AuditField::Target};
        if (!validIdentifier(input.requirementRef, auditReferenceCapacity, false))
            return AuditRefusal{AuditRefusalReason::InvalidIdentifier, AuditField::RequirementRef};
        if (!validIdentifier(input.riskRef, auditReferenceCapacity, false))
            return AuditRefusal{AuditRefusalReason::InvalidIdentifier, AuditField::RiskRef};
        if (!validIdentifier(input.correlationId, auditCorrelationCapacity, false))
            return AuditRefusal{AuditRefusalReason::InvalidIdentifier, AuditField::CorrelationId};
        if (!validIdentifier(streamId, auditStreamCapacity, true))
            return AuditRefusal{AuditRefusalReason::InvalidStream, AuditField::StreamId};
        return std::nullopt;
    }

    [[nodiscard]] constexpr AuditCategory category() const noexcept {
        return categoryValue;
    }
    [[nodiscard]] constexpr AuditPhase phase() const noexcept {
        return phaseValue;
    }
    [[nodiscard]] constexpr RawTime time() const noexcept {
        return timeValue;
    }
    [[nodiscard]] constexpr std::string_view action() const noexcept {
        return actionValue.view();
    }
    [[nodiscard]] constexpr std::string_view actor() const noexcept {
        return actorValue.view();
    }
    [[nodiscard]] constexpr std::string_view target() const noexcept {
        return targetValue.view();
    }
    [[nodiscard]] constexpr std::string_view requirementRef() const noexcept {
        return requirementValue.view();
    }
    [[nodiscard]] constexpr std::string_view riskRef() const noexcept {
        return riskValue.view();
    }
    [[nodiscard]] constexpr std::string_view correlationId() const noexcept {
        return correlationValue.view();
    }
    [[nodiscard]] constexpr std::string_view streamId() const noexcept {
        return streamValue.view();
    }
    [[nodiscard]] constexpr std::uint64_t sequence() const noexcept {
        return sequenceValue;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> sourceSequence() const noexcept {
        return sourceSequenceValue;
    }
    [[nodiscard]] constexpr std::string_view detail() const noexcept {
        return detailValue.view();
    }
    [[nodiscard]] constexpr bool detailTruncated() const noexcept {
        return shortenedValue;
    }

private:
    [[nodiscard]] static constexpr bool validIdentifier(std::string_view value, std::size_t capacity, bool required) noexcept {
        if (value.size() > capacity || (required && value.empty()))
            return false;
        for (char ch : value) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == ':' || ch == '/'
                  || ch == '-'))
                return false;
        }
        return true;
    }

    AuditCategory                          categoryValue = AuditCategory::Lifecycle;
    AuditPhase                             phaseValue    = AuditPhase::Requested;
    RawTime                                timeValue     = RawTime::unavailable();
    InlineString<auditActionCapacity>      actionValue;
    InlineString<auditActorCapacity>       actorValue;
    InlineString<auditTargetCapacity>      targetValue;
    InlineString<auditReferenceCapacity>   requirementValue;
    InlineString<auditReferenceCapacity>   riskValue;
    InlineString<auditCorrelationCapacity> correlationValue;
    InlineString<auditStreamCapacity>      streamValue;
    std::uint64_t                          sequenceValue = 0;
    std::optional<std::uint64_t>           sourceSequenceValue;
    InlineString<auditDetailCapacity>      detailValue;
    bool                                   shortenedValue = false;
};

static_assert(std::is_trivially_copyable_v<AuditEvent>);
static_assert(std::is_trivially_copyable_v<AuditWriteResult>);

}  // namespace mddlog::core
