/** @brief Owned diagnostic invariants shared by governed producers and allocating adapters. */
export module mddlog.core.diagnosticcontext;

import std;
export import mddlog.core.record;

export namespace mddlog::core {

/** @brief Views copied exactly by DiagnosticContext::create(); no view is retained. */
struct DiagnosticContextOptions {
    std::string_view component;
    std::string_view operationId;
    std::string_view correlationId;
};

/** @brief A derived operation replaces these two fields without mutating its parent. */
struct OperationContextOptions {
    std::string_view operationId;
    std::string_view correlationId;
};

/**
 * @brief Immutable-by-interface, bounded diagnostic identifiers, independent of destination and time.
 *
 * Factories return an exact field/refusal instead of shortening identifiers. Copies own their bytes;
 * accessor views remain valid only while this value exists and is not replaced. A context can be
 * shared for reading; it does not grant permission to share an SPSC producer between threads.
 */
class DiagnosticContext {
public:
    [[nodiscard]] static constexpr std::expected<DiagnosticContext, Refusal> create(DiagnosticContextOptions options) noexcept {
        DiagnosticContext next;
        if (!next.componentValue.assignExact(options.component))
            return std::unexpected(Refusal{.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::Component});
        if (!next.operationValue.assignExact(options.operationId))
            return std::unexpected(Refusal{.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::OperationId});
        if (!next.correlationValue.assignExact(options.correlationId))
            return std::unexpected(Refusal{.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::CorrelationId});
        return next;
    }

    /** @brief Create an independent operation context, preserving only the parent's component. */
    [[nodiscard]] constexpr std::expected<DiagnosticContext, Refusal> withOperation(OperationContextOptions options) const noexcept {
        return create({.component = component(), .operationId = options.operationId, .correlationId = options.correlationId});
    }

    [[nodiscard]] constexpr std::string_view component() const noexcept {
        return componentValue.view();
    }
    [[nodiscard]] constexpr std::string_view operationId() const noexcept {
        return operationValue.view();
    }
    [[nodiscard]] constexpr std::string_view correlationId() const noexcept {
        return correlationValue.view();
    }

private:
    constexpr DiagnosticContext() noexcept = default;
    InlineString<componentCapacity>     componentValue;
    InlineString<operationIdCapacity>   operationValue;
    InlineString<correlationIdCapacity> correlationValue;
};

static_assert(std::is_trivially_copyable_v<DiagnosticContext>);
static_assert(sizeof(DiagnosticContext) <= componentCapacity + operationIdCapacity + correlationIdCapacity + (3 * sizeof(std::uint16_t)));

}  // namespace mddlog::core
