/**
 * @brief Static Log helper for simplified logging syntax - C++23 Module
 */

export module mddlog.log;

import std;
import mddlog.core.loglevel;
import mddlog.core.logrecord;
import mddlog.core.logger;
import mddlog.sinks.sink;
import mddlog.sinks.console;

export namespace mddlog {

/**
 * @brief Static Log helper class providing simplified global logging interface
 *
 * This class provides a convenient static interface for logging without
 * requiring explicit logger instantiation. Perfect for simple applications
 * and medical device prototyping.
 *
 * @code
 * using namespace mddlog;
 * Log::setMinLevel(LogLevel::Info);
 * Log::info("System started");
 * Log::warn("Temperature elevated");
 * @endcode
 */
class Log {
public:
    /**
     * @brief Initialize the global logger with console sink
     * @param loggerName Name for the global logger (default: "GlobalLogger")
     * @param enableColors Enable colored console output (default: true)
     * @param asyncLogging Enable asynchronous logging (default: true)
     */
    static void initialize(std::string_view loggerName = "GlobalLogger", bool enableColors = true, bool asyncLogging = true) {
        std::scoped_lock lock(mutex);

        if (!globalLogger) {
            globalLogger = std::make_shared<core::SimpleLogger>(loggerName, asyncLogging);

            // Add default console sink
            auto consoleSink = sinks::createConsoleSink(enableColors, true);
            globalLogger->addSink(consoleSink);

            // Set default level to Info for medical devices
            globalLogger->setMinLevel(core::LogLevel::Info);
        }
    }

    /**
     * @brief Set the minimum log level
     * @param level Minimum level to log
     */
    static void setMinLevel(core::LogLevel level) {
        snapshot()->setMinLevel(level);
    }

    /**
     * @brief Add a custom sink to the global logger
     * @param sink Sink to add
     */
    static void addSink(sinks::SinkPtr sink) {
        snapshot()->addSink(std::move(sink));
    }

    /**
     * @brief Enable or disable the global logger
     * @param enabled New enabled state
     */
    static void setEnabled(bool enabled) {
        snapshot()->setEnabled(enabled);
    }

    /**
     * @brief Flush all sinks
     */
    static void flush() {
        snapshot()->flush();
    }

    // Convenience logging methods

    /** @brief Log a trace message */
    static void trace(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        snapshot()->trace(message, category, loc);
    }

    /** @brief Log a debug message */
    static void debug(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        snapshot()->debug(message, category, loc);
    }

    /** @brief Log an info message */
    static void info(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        snapshot()->info(message, category, loc);
    }

    /** @brief Log a warning message */
    static void warn(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        snapshot()->warn(message, category, loc);
    }

    /** @brief Log an error message */
    static void error(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        snapshot()->error(message, category, loc);
    }

    /** @brief Log a fatal message */
    static void fatal(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        snapshot()->fatal(message, category, loc);
    }

    /**
     * @brief Log a medical compliance record
     * @param level Log level
     * @param message Log message
     * @param category Category/component name
     * @param userId User identifier
     * @param sessionId Session identifier
     * @param deviceId Device identifier
     * @param loc Caller's source location (auto-filled)
     */
    static void logMedical(core::LogLevel              level,
                           std::string_view            message,
                           std::string_view            category,
                           std::string_view            userId,
                           std::string_view            sessionId,
                           std::string_view            deviceId,
                           const std::source_location& loc = std::source_location::current()) {
        snapshot()->logMedical(level, message, category, userId, sessionId, deviceId, loc);
    }

    /**
     * @brief Log an audit event
     * @param message Audit message
     * @param eventType Type of audit event
     * @param userId User who triggered the event
     * @param deviceId Device identifier
     * @param riskLevel Associated risk level
     * @param loc Caller's source location (auto-filled)
     */
    static void logAudit(std::string_view            message,
                         std::string_view            eventType,
                         std::string_view            userId,
                         std::string_view            deviceId,
                         std::string_view            riskLevel = "",
                         const std::source_location& loc       = std::source_location::current()) {
        snapshot()->logAudit(message, eventType, userId, deviceId, riskLevel, loc);
    }

    /**
     * @brief Get a shared handle to the global logger
     * @return Shared ownership of the global logger (auto-initializes if needed)
     *
     * Returns a shared_ptr rather than a reference: a reference could dangle if shutdown()
     * ran on another thread between the caller obtaining it and using it, whereas shared
     * ownership keeps the logger alive for as long as the caller's copy is.
     */
    static std::shared_ptr<core::SimpleLogger> getLogger() {
        return snapshot();
    }

    /**
     * @brief Check if the global logger is initialized
     * @return True if initialized
     */
    static bool isInitialized() noexcept {
        std::scoped_lock lock(mutex);
        return globalLogger != nullptr;
    }

    /**
     * @brief Shutdown the global logger
     *
     * This will flush all pending messages and clean up resources.
     * After calling this, the logger can be re-initialized if needed.
     *
     * Resetting the shared static handle does not affect a call already in flight on another
     * thread: that call holds its own shared_ptr copy (see snapshot()), so the logger object
     * itself stays alive until every such call has returned.
     */
    static void shutdown() {
        std::shared_ptr<core::SimpleLogger> toShutdown;
        {
            std::scoped_lock lock(mutex);
            toShutdown = std::move(globalLogger);
            globalLogger.reset();
        }
        if (toShutdown) {
            toShutdown->flush();
        }
    }

    // Delete copy/move constructors for static class
    Log()                      = delete;
    ~Log()                     = default;
    Log(const Log&)            = delete;
    Log& operator=(const Log&) = delete;
    Log(Log&&)                 = delete;
    Log& operator=(Log&&)      = delete;

private:
    /**
     * @brief Ensure the global logger is initialized and return a shared handle to it
     *
     * Every public method routes through this rather than dereferencing globalLogger
     * directly: taking the shared_ptr copy while holding mutex is what prevents a concurrent
     * shutdown() from destroying the logger out from under a call already in progress.
     */
    static std::shared_ptr<core::SimpleLogger> snapshot() {
        std::scoped_lock lock(mutex);
        if (!globalLogger) {
            // Auto-initialize with default settings
            globalLogger = std::make_shared<core::SimpleLogger>("GlobalLogger", true);

            // Add default console sink
            auto consoleSink = sinks::createConsoleSink(true, true);
            globalLogger->addSink(consoleSink);

            // Set default level to Info for medical devices
            globalLogger->setMinLevel(core::LogLevel::Info);
        }
        return globalLogger;
    }

    static inline std::shared_ptr<core::SimpleLogger> globalLogger;
    static inline std::mutex                          mutex;
};

}  // namespace mddlog
