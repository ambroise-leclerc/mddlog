/** @brief Owned host event descriptions and audit operation context, validated without publication. */
export module mddlog.core.auditcontext;

import std;
export import mddlog.core.auditevent;

export namespace mddlog::core {

/** @brief Host vocabulary and optional semantic references, copied by AuditDescription::create(). */
struct AuditDescriptionOptions {
    AuditCategory    category = AuditCategory::Lifecycle;
    std::string_view action;
    std::string_view requirementRef;
    std::string_view riskRef;
};

/** @brief Reusable host vocabulary; contains no phase, time, action result or stream identity. */
class AuditDescription {
public:
    [[nodiscard]] static constexpr std::expected<AuditDescription, AuditRefusal> create(AuditDescriptionOptions options) noexcept {
        if (auto failure = AuditEvent::validateIdentifier(AuditField::Action, options.action))
            return std::unexpected(*failure);
        if (auto failure = AuditEvent::validateIdentifier(AuditField::RequirementRef, options.requirementRef))
            return std::unexpected(*failure);
        if (auto failure = AuditEvent::validateIdentifier(AuditField::RiskRef, options.riskRef))
            return std::unexpected(*failure);
        if (isReservedAuditAction(options.action))
            return std::unexpected(AuditRefusal{.reason = AuditRefusalReason::ReservedAction, .field = AuditField::Action});
        AuditDescription next;
        next.categoryValue = options.category;
        (void)next.actionValue.assignExact(options.action);
        (void)next.requirementValue.assignExact(options.requirementRef);
        (void)next.riskValue.assignExact(options.riskRef);
        return next;
    }
    [[nodiscard]] constexpr AuditCategory category() const noexcept {
        return categoryValue;
    }
    [[nodiscard]] constexpr std::string_view action() const noexcept {
        return actionValue.view();
    }
    [[nodiscard]] constexpr std::string_view requirementRef() const noexcept {
        return requirementValue.view();
    }
    [[nodiscard]] constexpr std::string_view riskRef() const noexcept {
        return riskValue.view();
    }

private:
    constexpr AuditDescription() noexcept              = default;
    AuditCategory                        categoryValue = AuditCategory::Lifecycle;
    InlineString<auditActionCapacity>    actionValue;
    InlineString<auditReferenceCapacity> requirementValue;
    InlineString<auditReferenceCapacity> riskValue;
};

/** @brief Operation identities copied exactly by AuditContext::create(); target is mandatory. */
struct AuditContextOptions {
    std::string_view actor;
    std::string_view target;
    std::string_view correlationId;
};

/**
 * @brief Independent, owned audit identities; creating or destroying a context never publishes.
 * @note Copies own their bytes. Accessor views require this value to exist and not be replaced.
 *       Actor and correlation may be empty; target and every nonempty identifier follow AuditEvent.
 */
class AuditContext {
public:
    [[nodiscard]] static constexpr std::expected<AuditContext, AuditRefusal> create(AuditContextOptions options) noexcept {
        if (auto failure = AuditEvent::validateIdentifier(AuditField::Actor, options.actor))
            return std::unexpected(*failure);
        if (auto failure = AuditEvent::validateIdentifier(AuditField::Target, options.target))
            return std::unexpected(*failure);
        if (auto failure = AuditEvent::validateIdentifier(AuditField::CorrelationId, options.correlationId))
            return std::unexpected(*failure);
        AuditContext next;
        (void)next.actorValue.assignExact(options.actor);
        (void)next.targetValue.assignExact(options.target);
        (void)next.correlationValue.assignExact(options.correlationId);
        return next;
    }
    [[nodiscard]] constexpr std::string_view actor() const noexcept {
        return actorValue.view();
    }
    [[nodiscard]] constexpr std::string_view target() const noexcept {
        return targetValue.view();
    }
    [[nodiscard]] constexpr std::string_view correlationId() const noexcept {
        return correlationValue.view();
    }

private:
    constexpr AuditContext() noexcept = default;
    InlineString<auditActorCapacity>       actorValue;
    InlineString<auditTargetCapacity>      targetValue;
    InlineString<auditCorrelationCapacity> correlationValue;
};

static_assert(std::is_trivially_copyable_v<AuditDescription> && std::is_trivially_copyable_v<AuditContext>);
static_assert(sizeof(AuditDescription) <= auditActionCapacity + (2 * auditReferenceCapacity) + (4 * sizeof(std::uint16_t)));
static_assert(sizeof(AuditContext) <= auditActorCapacity + auditTargetCapacity + auditCorrelationCapacity + (4 * sizeof(std::uint16_t)));

}  // namespace mddlog::core
