/** @brief Local application component: stock changes with explicit host audit refusal policy. */
#ifndef MDDLOG_EXAMPLES_INVENTORYWORKER_HPP
#define MDDLOG_EXAMPLES_INVENTORYWORKER_HPP

// Import std, mddlog.core.governedbinding and mddlog.core.auditbinding before this header.
namespace inventory {
using namespace mddlog::core;

/** @brief Mutation and audit admission are separate facts; an absent outcome means no action ran. */
struct [[nodiscard]] AdjustmentResult {
    AuditWriteResult                request;
    std::optional<AuditWriteResult> outcome;
    std::optional<WriteResult>      diagnostic;
    bool                            changed = false;
};

/**
 * @brief Adjust a local stock count; a refused request blocks mutation, a refused outcome cannot undo it.
 * @note The host prepares both bindings and keeps their rings alive through all worker copies and joins.
 *       One thread owns this mutable component. Consumers, retries and storage remain outside apply().
 */
template <std::size_t DiagnosticCapacity, std::size_t AuditCapacity>
class InventoryWorker {
public:
    InventoryWorker(GovernedBinding<DiagnosticCapacity> diagnosticBinding, AuditBinding<AuditCapacity> auditBinding) noexcept
        : logger(diagnosticBinding), audit(auditBinding) {}

    /** @brief Host supplies both observation times; invalid stock changes explicitly emit Failed. */
    [[nodiscard]] AdjustmentResult apply(int delta, RawTime requestedAt, RawTime observedAt) noexcept {
        const auto request = audit.record(AuditPhase::Requested, requestedAt);
        if (!request.wasAdmitted())
            return {.request = request};
        const auto next  = static_cast<std::int64_t>(stock) + delta;
        const bool valid = next >= 0 && next <= std::numeric_limits<int>::max();
        if (valid)
            stock = static_cast<int>(next);
        const auto outcome    = audit.record(valid ? AuditPhase::Executed : AuditPhase::Failed, observedAt, {.sourceSequence = request.sequence()});
        const auto diagnostic = logger.info(observedAt, valid ? "Stock adjusted" : "Stock change rejected");
        return {.request = request, .outcome = outcome, .diagnostic = diagnostic, .changed = valid};
    }

    [[nodiscard]] int quantity() const noexcept {
        return stock;
    }

private:
    GovernedBinding<DiagnosticCapacity> logger;
    AuditBinding<AuditCapacity>         audit;
    int                                 stock = 0;
};

/** @brief Equivalent low-level business function, for the same concrete stock-change policy. */
template <std::size_t Capacity>
[[nodiscard]] AdjustmentResult adjustBefore(RingLog<Capacity>&       ring,
                                            AuditRing<Capacity>&     auditRing,
                                            const DiagnosticContext& context,
                                            const AuditDescription&  description,
                                            const AuditContext&      auditContext,
                                            int&                     stock,
                                            int                      delta,
                                            RawTime                  requestedAt,
                                            RawTime                  observedAt) noexcept {
    const auto request = auditRing.tryRecord({.category       = description.category(),
                                              .phase          = AuditPhase::Requested,
                                              .time           = requestedAt,
                                              .action         = description.action(),
                                              .actor          = auditContext.actor(),
                                              .target         = auditContext.target(),
                                              .requirementRef = description.requirementRef(),
                                              .riskRef        = description.riskRef(),
                                              .correlationId  = auditContext.correlationId()});
    if (!request.wasAdmitted())
        return {.request = request};
    const auto next  = static_cast<std::int64_t>(stock) + delta;
    const bool valid = next >= 0 && next <= std::numeric_limits<int>::max();
    if (valid)
        stock = static_cast<int>(next);
    const auto outcome    = auditRing.tryRecord({.category       = description.category(),
                                                 .phase          = valid ? AuditPhase::Executed : AuditPhase::Failed,
                                                 .time           = observedAt,
                                                 .action         = description.action(),
                                                 .actor          = auditContext.actor(),
                                                 .target         = auditContext.target(),
                                                 .requirementRef = description.requirementRef(),
                                                 .riskRef        = description.riskRef(),
                                                 .correlationId  = auditContext.correlationId(),
                                                 .sourceSequence = request.sequence()});
    const auto diagnostic = ring.tryWrite({.time          = observedAt,
                                           .location      = std::source_location::current(),
                                           .message       = valid ? "Stock adjusted" : "Stock change rejected",
                                           .component     = context.component(),
                                           .operationId   = context.operationId(),
                                           .correlationId = context.correlationId()});
    return {.request = request, .outcome = outcome, .diagnostic = diagnostic, .changed = valid};
}
}  // namespace inventory

#endif  // MDDLOG_EXAMPLES_INVENTORYWORKER_HPP
