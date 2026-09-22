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
     * Log::setMinLevel(LogLevel::INFO);
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
        static void initialize(std::string_view loggerName = "GlobalLogger", 
                             bool enableColors = true, 
                             bool asyncLogging = true) {
            std::lock_guard<std::mutex> lock(mutex_);
            
            if (!globalLogger_) {
                globalLogger_ = std::make_unique<core::SimpleLogger>(loggerName, asyncLogging);
                
                // Add default console sink
                auto consoleSink = sinks::createConsoleSink(enableColors, true);
                globalLogger_->addSink(consoleSink);
                
                // Set default level to INFO for medical devices
                globalLogger_->setMinLevel(core::LogLevel::INFO);
            }
        }

        /**
         * @brief Set the minimum log level
         * @param level Minimum level to log
         */
        static void setMinLevel(core::LogLevel level) {
            ensureInitialized();
            globalLogger_->setMinLevel(level);
        }

        /**
         * @brief Add a custom sink to the global logger
         * @param sink Sink to add
         */
        static void addSink(sinks::SinkPtr sink) {
            ensureInitialized();
            globalLogger_->addSink(std::move(sink));
        }

        /**
         * @brief Enable or disable the global logger
         * @param enabled New enabled state
         */
        static void setEnabled(bool enabled) {
            ensureInitialized();
            globalLogger_->setEnabled(enabled);
        }

        /**
         * @brief Flush all sinks
         */
        static void flush() {
            ensureInitialized();
            globalLogger_->flush();
        }

        // Convenience logging methods
        
        /** @brief Log a trace message */
        static void trace(std::string_view message, std::string_view category = "default") {
            ensureInitialized();
            globalLogger_->trace(message, category);
        }

        /** @brief Log a debug message */
        static void debug(std::string_view message, std::string_view category = "default") {
            ensureInitialized();
            globalLogger_->debug(message, category);
        }

        /** @brief Log an info message */
        static void info(std::string_view message, std::string_view category = "default") {
            ensureInitialized();
            globalLogger_->info(message, category);
        }

        /** @brief Log a warning message */
        static void warn(std::string_view message, std::string_view category = "default") {
            ensureInitialized();
            globalLogger_->warn(message, category);
        }

        /** @brief Log an error message */
        static void error(std::string_view message, std::string_view category = "default") {
            ensureInitialized();
            globalLogger_->error(message, category);
        }

        /** @brief Log a fatal message */
        static void fatal(std::string_view message, std::string_view category = "default") {
            ensureInitialized();
            globalLogger_->fatal(message, category);
        }

        /**
         * @brief Log a medical compliance record
         * @param level Log level
         * @param message Log message
         * @param category Category/component name
         * @param userId User identifier
         * @param sessionId Session identifier
         * @param deviceId Device identifier
         */
        static void logMedical(core::LogLevel level,
                              std::string_view message,
                              std::string_view category,
                              std::string_view userId,
                              std::string_view sessionId,
                              std::string_view deviceId) {
            ensureInitialized();
            globalLogger_->logMedical(level, message, category, userId, sessionId, deviceId);
        }

        /**
         * @brief Log an audit event
         * @param message Audit message
         * @param eventType Type of audit event
         * @param userId User who triggered the event
         * @param deviceId Device identifier
         * @param riskLevel Associated risk level
         */
        static void logAudit(std::string_view message,
                            std::string_view eventType,
                            std::string_view userId,
                            std::string_view deviceId,
                            std::string_view riskLevel = "") {
            ensureInitialized();
            globalLogger_->logAudit(message, eventType, userId, deviceId, riskLevel);
        }

        /**
         * @brief Get the global logger instance
         * @return Reference to the global logger (auto-initializes if needed)
         */
        static core::SimpleLogger& getLogger() {
            ensureInitialized();
            return *globalLogger_;
        }

        /**
         * @brief Check if the global logger is initialized
         * @return True if initialized
         */
        static bool isInitialized() noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            return globalLogger_ != nullptr;
        }

        /**
         * @brief Shutdown the global logger
         * 
         * This will flush all pending messages and clean up resources.
         * After calling this, the logger can be re-initialized if needed.
         */
        static void shutdown() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (globalLogger_) {
                globalLogger_->flush();
                globalLogger_.reset();
            }
        }

        // Delete copy/move constructors for static class
        Log() = delete;
        Log(const Log&) = delete;
        Log& operator=(const Log&) = delete;
        Log(Log&&) = delete;
        Log& operator=(Log&&) = delete;

    private:
        /**
         * @brief Ensure the global logger is initialized
         */
        static void ensureInitialized() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!globalLogger_) {
                // Auto-initialize with default settings
                globalLogger_ = std::make_unique<core::SimpleLogger>("GlobalLogger", true);
                
                // Add default console sink
                auto consoleSink = sinks::createConsoleSink(true, true);
                globalLogger_->addSink(consoleSink);
                
                // Set default level to INFO for medical devices
                globalLogger_->setMinLevel(core::LogLevel::INFO);
            }
        }

        static inline std::unique_ptr<core::SimpleLogger> globalLogger_;
        static inline std::mutex mutex_;
    };

} // namespace mddlog