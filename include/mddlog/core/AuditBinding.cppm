/** @brief Explicit audit phases admitted through a context-owning SPSC producer binding. */
export module mddlog.core.auditbinding;

import std;
export import mddlog.core.auditcontext;
export import mddlog.core.auditring;

export namespace mddlog::core {

/** @brief Facts specific to this emission; views are read synchronously and copied on admission. */
struct AuditEventOptions {
    std::string_view             detail;
    std::optional<std::uint64_t> sourceSequence;
};

/**
 * @brief Own description and context, borrowing one producer's audit ring without locks or allocation.
 * @tparam Capacity The existing ring capacity, not a new queue.
 * @note The ring must outlive every binding copy. Exactly one producer emits per ring. Stream
 *       identity and sequence remain owned by the ring; invalid streams are refused on emission.
 *       Construction/destruction emit nothing. Admission proves only an in-memory copy, never
 *       execution, confirmation, hand-off or durability. No diagnostic filter affects this lane.
 */
template <std::size_t Capacity>
class AuditBinding {
public:
    constexpr AuditBinding(AuditRing<Capacity>& destination, AuditDescription eventDescription, AuditContext initialContext) noexcept
        : ring(destination), description(eventDescription), context(initialContext) {}
    AuditBinding(AuditRing<Capacity>&&, AuditDescription, AuditContext) = delete;

    /** @brief Explicitly record host facts; phase and host time have no defaults. */
    [[nodiscard]] AuditWriteResult record(AuditPhase phase, RawTime time, AuditEventOptions options = {}) noexcept {
        return ring.get().tryRecord({.category       = description.category(),
                                     .phase          = phase,
                                     .time           = time,
                                     .action         = description.action(),
                                     .actor          = context.actor(),
                                     .target         = context.target(),
                                     .requirementRef = description.requirementRef(),
                                     .riskRef        = description.riskRef(),
                                     .correlationId  = context.correlationId(),
                                     .sourceSequence = options.sourceSequence,
                                     .detail         = options.detail});
    }

private:
    std::reference_wrapper<AuditRing<Capacity>> ring;
    AuditDescription                            description;
    AuditContext                                context;
};

static_assert(std::is_trivially_copyable_v<AuditBinding<1>>);
static_assert(sizeof(AuditBinding<1>) <= sizeof(AuditDescription) + sizeof(AuditContext) + (2 * sizeof(void*)));

}  // namespace mddlog::core
