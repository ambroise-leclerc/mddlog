/** @brief Contextual diagnostic emission into one borrowed SPSC ring. */
export module mddlog.core.governedbinding;

import std;
export import mddlog.core.diagnosticcontext;
export import mddlog.core.ring;

export namespace mddlog::core {

/**
 * @brief A producer-local diagnostic binding that owns context and borrows its destination.
 * @tparam Capacity Capacity of the producer's existing RingLog.
 * @note The ring must outlive every binding copy. Exactly one producer emits on the ring;
 *       copying the binding never changes SPSC. Destruction performs no emission or drain.
 */
template <std::size_t Capacity>
class GovernedBinding {
public:
    constexpr GovernedBinding(RingLog<Capacity>& destination, DiagnosticContext initialContext) noexcept : ring(destination), context(initialContext) {}
    GovernedBinding(RingLog<Capacity>&&, DiagnosticContext) = delete;

    /** @brief Copy one message and context into the ring, preserving caller source and host time. */
    [[nodiscard]] WriteResult
    log(LogLevel level, RawTime time, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
        return ring.get().tryWrite({.level         = level,
                                    .time          = time,
                                    .location      = location,
                                    .message       = message,
                                    .component     = context.component(),
                                    .operationId   = context.operationId(),
                                    .correlationId = context.correlationId()});
    }

    /** @brief Emit a trace diagnostic; admission and truncation remain explicit. */
    [[nodiscard]] WriteResult trace(RawTime time, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
        return log(LogLevel::Trace, time, message, location);
    }

    /** @brief Emit a debug diagnostic; admission and truncation remain explicit. */
    [[nodiscard]] WriteResult debug(RawTime time, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
        return log(LogLevel::Debug, time, message, location);
    }

    /** @brief Emit an info diagnostic; admission and truncation remain explicit. */
    [[nodiscard]] WriteResult info(RawTime time, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
        return log(LogLevel::Info, time, message, location);
    }

    /** @brief Emit a warn diagnostic; admission and truncation remain explicit. */
    [[nodiscard]] WriteResult warn(RawTime time, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
        return log(LogLevel::Warn, time, message, location);
    }

    /** @brief Emit an error diagnostic; admission and truncation remain explicit. */
    [[nodiscard]] WriteResult error(RawTime time, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
        return log(LogLevel::Error, time, message, location);
    }

    /** @brief Emit a fatal diagnostic; admission and truncation remain explicit. */
    [[nodiscard]] WriteResult fatal(RawTime time, std::string_view message, std::source_location location = std::source_location::current()) noexcept {
        return log(LogLevel::Fatal, time, message, location);
    }

private:
    std::reference_wrapper<RingLog<Capacity>> ring;
    DiagnosticContext                         context;
};

static_assert(std::is_trivially_copyable_v<GovernedBinding<1>>);
static_assert(sizeof(GovernedBinding<1>) <= sizeof(DiagnosticContext) + (2 * sizeof(void*)));

}  // namespace mddlog::core
