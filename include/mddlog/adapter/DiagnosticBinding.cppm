/** @brief One injected contextual diagnostic interface for the existing text and object-sink loggers. */
export module mddlog.adapter.diagnosticbinding;

import std;
export import mddlog.core.diagnosticcontext;
export import mddlog.adapter.textlogger;
export import mddlog.adapter.logger;

export namespace mddlog::adapter {

/**
 * @brief Own context and borrow a diagnostic logger; formatting and delivery stay in the adapter.
 * @tparam Logger TextLogger or core::SimpleLogger, whose existing filters and lifetime contracts apply.
 * @note The destination outlives this binding. SimpleLogger stores structured fields; TextLogger
 *       renders [component:operation:correlation] as text. Ordinary arguments are evaluated eagerly.
 *       logLazy checks the filter before invoking its factory once. A concurrent disable may prevent
 *       delivery after construction; factory creation itself is still eager and may allocate/throw.
 */
template <typename Logger>
    requires(std::same_as<Logger, TextLogger> || std::same_as<Logger, core::SimpleLogger>)
class DiagnosticBinding {
public:
    DiagnosticBinding(Logger& destination, core::DiagnosticContext initialContext) noexcept : logger(destination), context(initialContext) {}
    DiagnosticBinding(Logger&&, core::DiagnosticContext) = delete;

    [[nodiscard]] bool is(core::LogLevel level) const noexcept {
        return logger.get().is(level);
    }

    /** @brief Emit using the existing filter, capturing source at this method's caller. */
    void log(core::LogLevel level, std::string_view message, std::source_location location = std::source_location::current()) {
        if (!is(level))
            return;
        if constexpr (std::same_as<Logger, TextLogger>) {
            logger.get().write(level, std::format("[{}:{}:{}] {}", context.component(), context.operationId(), context.correlationId(), message), location);
        } else {
            logger.get().log(level, message, context, location);
        }
    }

    /** @brief Evaluate a synchronous message factory only after a filter snapshot, at most once. */
    template <typename Factory>
        requires std::invocable<Factory> && std::convertible_to<std::invoke_result_t<Factory>, std::string_view>
    void logLazy(core::LogLevel level, Factory&& factory, std::source_location location = std::source_location::current()) {
        if (!is(level))
            return;
        const auto message = std::invoke(std::forward<Factory>(factory));
        log(level, message, location);
    }

    /** @brief Lazy Debug diagnostic, with caller source forwarded through every layer. */
    template <typename Factory>
        requires std::invocable<Factory> && std::convertible_to<std::invoke_result_t<Factory>, std::string_view>
    void debugLazy(Factory&& factory, std::source_location location = std::source_location::current()) {
        logLazy(core::LogLevel::Debug, std::forward<Factory>(factory), location);
    }

    /** @brief Emit a trace diagnostic with this binding's context. */
    void trace(std::string_view message, std::source_location location = std::source_location::current()) {
        log(core::LogLevel::Trace, message, location);
    }

    /** @brief Emit a debug diagnostic with this binding's context. */
    void debug(std::string_view message, std::source_location location = std::source_location::current()) {
        log(core::LogLevel::Debug, message, location);
    }

    /** @brief Emit a info diagnostic with this binding's context. */
    void info(std::string_view message, std::source_location location = std::source_location::current()) {
        log(core::LogLevel::Info, message, location);
    }

    /** @brief Emit a warn diagnostic with this binding's context. */
    void warn(std::string_view message, std::source_location location = std::source_location::current()) {
        log(core::LogLevel::Warn, message, location);
    }

    /** @brief Emit a error diagnostic with this binding's context. */
    void error(std::string_view message, std::source_location location = std::source_location::current()) {
        log(core::LogLevel::Error, message, location);
    }

    /** @brief Emit a fatal diagnostic with this binding's context. */
    void fatal(std::string_view message, std::source_location location = std::source_location::current()) {
        log(core::LogLevel::Fatal, message, location);
    }

private:
    std::reference_wrapper<Logger> logger;
    core::DiagnosticContext        context;
};

}  // namespace mddlog::adapter
