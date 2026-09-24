/**
 * @brief Logger implementation for medical device logging - C++23 Module
 *
 * Adapter-zone module (ADR-001 Decision 6): SimpleLogger stores sinks and an unbounded queue,
 * both allocating. The namespace stays mddlog::core for now: only the module name and file
 * location moved (#32), not the class itself.
 */

export module mddlog.adapter.logger;

import std;
import mddlog.core.loglevel;
import mddlog.adapter.logrecord;
import mddlog.sinks.sink;

export namespace mddlog::core {

/**
 * @brief Simplified logger class with basic threading support
 *
 * This logger provides thread-safe logging capabilities for medical devices
 * using standard C++ threading primitives for maximum compatibility.
 */
class SimpleLogger {
public:
    using SinkPtr       = sinks::SinkPtr;
    using SinkContainer = std::vector<SinkPtr>;

    /**
     * @brief Constructor
     * @param loggerName Logger name/identifier
     * @param enableAsyncLogging Enable asynchronous logging (default: true)
     */
    explicit SimpleLogger(std::string_view loggerName, bool enableAsyncLogging = true)
        : name(loggerName), asyncLogging(enableAsyncLogging), enabled(true), minLevel(LogLevel::Info), shuttingDown(false) {
        if (asyncLogging) {
            // Start async logging thread
            asyncThread = std::thread([this]() {
                asyncLoggerThread();
            });
        }
    }

    /**
     * @brief Destructor - ensures proper cleanup
     */
    ~SimpleLogger() {
        shutdown();
    }

    // Disable copy and move for thread safety
    SimpleLogger(const SimpleLogger&)            = delete;
    SimpleLogger& operator=(const SimpleLogger&) = delete;
    SimpleLogger(SimpleLogger&&)                 = delete;
    SimpleLogger& operator=(SimpleLogger&&)      = delete;

    /**
     * @brief Add a sink to the logger
     * @param sink Sink to add
     */
    void addSink(SinkPtr sink) {
        if (!sink)
            return;

        std::scoped_lock lock(sinksMutex);
        sinks.push_back(std::move(sink));
    }

    /**
     * @brief Remove a sink from the logger
     * @param sinkName Name of sink to remove
     */
    void removeSink(std::string_view sinkName) {
        std::scoped_lock lock(sinksMutex);
        std::erase_if(sinks, [sinkName](const SinkPtr& sink) {
            return sink->getName() == sinkName;
        });
    }

    /**
     * @brief Clear all sinks
     */
    void clearSinks() {
        std::scoped_lock lock(sinksMutex);
        sinks.clear();
    }

    /**
     * @brief Log a message with specified level
     * @param level Log level
     * @param message Log message
     * @param category Category/component name
     * @param loc Caller's source location (auto-filled)
     *
     * @c loc must be an explicit parameter here rather than relying on LogRecord's own
     * defaulted @c std::source_location::current(): a default argument is evaluated at the
     * call site of the function that declares it, so without this parameter every record
     * would capture this line inside Logger, not the application's call to log()/info()/etc.
     */
    void
    log(LogLevel level, std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        if (!shouldLog(level))
            return;

        LogRecord record(level, message, category, loc);
        processLogRecord(std::move(record));
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
    void logMedical(LogLevel                    level,
                    std::string_view            message,
                    std::string_view            category,
                    std::string_view            userId,
                    std::string_view            sessionId,
                    std::string_view            deviceId,
                    const std::source_location& loc = std::source_location::current()) {
        if (!shouldLog(level))
            return;

        LogRecord record(level, message, category, userId, sessionId, deviceId, loc);
        processLogRecord(std::move(record));
    }

    /**
     * @brief Log an audit event
     * @param message Audit message
     * @param eventType Type of audit event
     * @param userId User who triggered the event
     * @param deviceId Device identifier
     * @param riskLevel Associated risk level
     * @param loc Caller's source location (auto-filled)
     *
     * Deliberately does not call shouldLog(): an audit trail entry must not be silenceable by
     * disabling the logger or raising its minimum level, which would otherwise let a
     * misconfigured or maliciously reconfigured logger erase compliance evidence. The record
     * still goes through each sink's own shouldLog()/isEnabled() in writeToSinks() like any
     * other record - since Audit is the highest LogLevel, a sink's minimum-level filter can
     * never exclude it, but an explicitly disabled sink (Sink::setEnabled(false)) still will.
     */
    void logAudit(std::string_view            message,
                  std::string_view            eventType,
                  std::string_view            userId,
                  std::string_view            deviceId,
                  std::string_view            riskLevel = "",
                  const std::source_location& loc       = std::source_location::current()) {
        LogRecord record(LogLevel::Audit, message, "audit", userId, "", deviceId, loc);
        record.setAuditInfo(eventType, riskLevel);
        processLogRecord(std::move(record));
    }

    // Convenience methods for different log levels
    void trace(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Trace, message, category, loc);
    }

    void debug(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Debug, message, category, loc);
    }

    void info(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Info, message, category, loc);
    }

    void warn(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Warn, message, category, loc);
    }

    void error(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Error, message, category, loc);
    }

    void fatal(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Fatal, message, category, loc);
    }

    /**
     * @brief Flush all sinks
     */
    void flush() {
        if (asyncLogging) {
            // For async logging, add a flush command to the queue
            std::promise<void> flushPromise;
            auto               flushFuture = flushPromise.get_future();

            {
                std::scoped_lock lock(queueMutex);
                flushPromises.push(std::move(flushPromise));
            }
            queueCondition.notify_one();

            // Wait for flush to complete
            flushFuture.wait();
        } else {
            // Synchronous flush
            flushSinks();
        }
    }

    /**
     * @brief Check if logger is enabled
     */
    bool isEnabled() const noexcept {
        return enabled.load();
    }

    /**
     * @brief Enable or disable the logger
     */
    void setEnabled(bool value) noexcept {
        enabled.store(value);
    }

    /**
     * @brief Get minimum log level
     */
    LogLevel getMinLevel() const noexcept {
        return minLevel.load();
    }

    /**
     * @brief Set minimum log level
     */
    void setMinLevel(LogLevel level) noexcept {
        minLevel.store(level);
    }

    /**
     * @brief Get logger name
     */
    std::string_view getName() const noexcept {
        return name;
    }

    /**
     * @brief Check if asynchronous logging is enabled
     */
    bool isAsyncLogging() const noexcept {
        return asyncLogging;
    }

    /**
     * @brief Get number of sinks
     */
    std::size_t getSinkCount() const {
        std::scoped_lock lock(sinksMutex);
        return sinks.size();
    }

private:
    /**
     * @brief Check if a log level should be processed
     */
    bool shouldLog(LogLevel level) const noexcept {
        return enabled.load() && level >= minLevel.load();
    }

    /**
     * @brief Process a log record (sync or async)
     */
    void processLogRecord(LogRecord record) {
        if (asyncLogging) {
            // Add to async queue
            {
                std::scoped_lock lock(queueMutex);
                logQueue.push(std::move(record));
            }
            queueCondition.notify_one();
        } else {
            // Process synchronously
            writeToSinks(record);
        }
    }

    /**
     * @brief Write log record to all sinks
     */
    void writeToSinks(const LogRecord& record) {
        std::scoped_lock lock(sinksMutex);
        for (auto& sink : sinks) {
            if (sink && sink->shouldLog(record.level) && sink->isEnabled()) {
                try {
                    sink->write(record);
                } catch (...) {
                    // A throwing sink must not stop delivery to the remaining sinks, nor
                    // propagate into the async worker thread. The failure is still made
                    // explicit and observable (rather than silently swallowed) through the
                    // sink's own statistics.
                    sink->recordWriteFailure();
                }
            }
        }
    }

    /**
     * @brief Flush all sinks
     */
    void flushSinks() {
        std::scoped_lock lock(sinksMutex);
        for (auto& sink : sinks) {
            if (sink) {
                try {
                    sink->flush();
                } catch (...) {  // NOLINT(bugprone-empty-catch): a failing flush must never propagate from shutdown or flush()
                    // Ignore flush errors
                }
            }
        }
    }

    /**
     * @brief Async logger thread function
     */
    void asyncLoggerThread() {
        while (!shuttingDown.load()) {
            std::unique_lock<std::mutex> lock(queueMutex);

            // Wait for log records or flush requests
            queueCondition.wait(lock, [this] {
                return !logQueue.empty() || !flushPromises.empty() || shuttingDown.load();
            });

            // Process all queued log records
            while (!logQueue.empty()) {
                auto record = std::move(logQueue.front());
                logQueue.pop();
                lock.unlock();

                writeToSinks(record);

                lock.lock();
            }

            // Process flush requests
            while (!flushPromises.empty()) {
                auto promise = std::move(flushPromises.front());
                flushPromises.pop();
                lock.unlock();

                flushSinks();
                promise.set_value();

                lock.lock();
            }
        }

        // Process remaining items before shutdown
        std::scoped_lock lock(queueMutex);
        while (!logQueue.empty()) {
            writeToSinks(logQueue.front());
            logQueue.pop();
        }
        flushSinks();
    }

    /**
     * @brief Shutdown the logger
     */
    void shutdown() {
        if (shuttingDown.exchange(true))
            return;

        if (asyncThread.joinable()) {
            queueCondition.notify_all();
            asyncThread.join();
        }

        // Final flush
        flushSinks();
    }

    std::string           name;          ///< Logger name
    bool                  asyncLogging;  ///< Async logging enabled
    std::atomic<bool>     enabled;       ///< Logger enabled state
    std::atomic<LogLevel> minLevel;      ///< Minimum log level
    std::atomic<bool>     shuttingDown;  ///< Set once shutdown() has been entered

    // Sinks management
    mutable std::mutex sinksMutex;  ///< Sinks container mutex
    SinkContainer      sinks;       ///< Collection of sinks

    // Async logging
    std::thread             asyncThread;     ///< Async logging thread
    std::mutex              queueMutex;      ///< Queue mutex
    std::condition_variable queueCondition;  ///< Queue condition variable
    // Unbounded by design/limitation: a producer that logs faster than the sinks can drain
    // grows this queue without bound rather than blocking or dropping records. That gives the
    // current implementation no real-time delivery guarantee under sustained overload; a
    // bounded, real-time-safe queue is out of scope for this change (see the build/test issue
    // that introduced this comment) and tracked separately.
    std::queue<LogRecord>          logQueue;       ///< Log record queue
    std::queue<std::promise<void>> flushPromises;  ///< Flush promises queue
};

}  // namespace mddlog::core
