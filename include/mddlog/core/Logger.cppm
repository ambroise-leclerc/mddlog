/**
 * @brief Logger implementation for medical device logging - C++23 Module
 */

export module mddlog.core.logger;

import std;
import mddlog.core.loglevel;
import mddlog.core.logrecord;
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
        using SinkPtr = sinks::SinkPtr;
        using SinkContainer = std::vector<SinkPtr>;

        /**
         * @brief Constructor
         * @param name Logger name/identifier
         * @param asyncLogging Enable asynchronous logging (default: true)
         */
        explicit SimpleLogger(std::string_view name, bool asyncLogging = true)
            : name_(name)
            , asyncLogging_(asyncLogging)
            , enabled_(true)
            , minLevel_(LogLevel::INFO)
            , shutdown_(false) {
            
            if (asyncLogging_) {
                // Start async logging thread
                asyncThread_ = std::thread([this]() {
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
        SimpleLogger(const SimpleLogger&) = delete;
        SimpleLogger& operator=(const SimpleLogger&) = delete;
        SimpleLogger(SimpleLogger&&) = delete;
        SimpleLogger& operator=(SimpleLogger&&) = delete;

        /**
         * @brief Add a sink to the logger
         * @param sink Sink to add
         */
        void addSink(SinkPtr sink) {
            if (!sink) return;
            
            std::lock_guard<std::mutex> lock(sinksMutex_);
            sinks_.push_back(std::move(sink));
        }

        /**
         * @brief Remove a sink from the logger
         * @param sinkName Name of sink to remove
         */
        void removeSink(std::string_view sinkName) {
            std::lock_guard<std::mutex> lock(sinksMutex_);
            sinks_.erase(
                std::remove_if(sinks_.begin(), sinks_.end(),
                    [sinkName](const SinkPtr& sink) {
                        return sink->getName() == sinkName;
                    }),
                sinks_.end());
        }

        /**
         * @brief Clear all sinks
         */
        void clearSinks() {
            std::lock_guard<std::mutex> lock(sinksMutex_);
            sinks_.clear();
        }

        /**
         * @brief Log a message with specified level
         * @param level Log level
         * @param message Log message
         * @param category Category/component name
         */
        void log(LogLevel level, std::string_view message, std::string_view category = "default") {
            if (!shouldLog(level)) return;

            LogRecord record(level, message, category);
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
         */
        void logMedical(LogLevel level, 
                       std::string_view message,
                       std::string_view category,
                       std::string_view userId,
                       std::string_view sessionId,
                       std::string_view deviceId) {
            if (!shouldLog(level)) return;

            LogRecord record(level, message, category, userId, sessionId, deviceId);
            processLogRecord(std::move(record));
        }

        /**
         * @brief Log an audit event
         * @param message Audit message
         * @param eventType Type of audit event
         * @param userId User who triggered the event
         * @param deviceId Device identifier
         * @param riskLevel Associated risk level
         */
        void logAudit(std::string_view message,
                     std::string_view eventType,
                     std::string_view userId,
                     std::string_view deviceId,
                     std::string_view riskLevel = "") {
            LogRecord record(LogLevel::AUDIT, message, "audit", userId, "", deviceId);
            record.setAuditInfo(eventType, riskLevel);
            processLogRecord(std::move(record));
        }

        // Convenience methods for different log levels
        void trace(std::string_view message, std::string_view category = "default") {
            log(LogLevel::TRACE, message, category);
        }

        void debug(std::string_view message, std::string_view category = "default") {
            log(LogLevel::DEBUG, message, category);
        }

        void info(std::string_view message, std::string_view category = "default") {
            log(LogLevel::INFO, message, category);
        }

        void warn(std::string_view message, std::string_view category = "default") {
            log(LogLevel::WARN, message, category);
        }

        void error(std::string_view message, std::string_view category = "default") {
            log(LogLevel::ERROR, message, category);
        }

        void fatal(std::string_view message, std::string_view category = "default") {
            log(LogLevel::FATAL, message, category);
        }

        /**
         * @brief Flush all sinks
         */
        void flush() {
            if (asyncLogging_) {
                // For async logging, add a flush command to the queue
                std::promise<void> flushPromise;
                auto flushFuture = flushPromise.get_future();
                
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    flushPromises_.push(std::move(flushPromise));
                }
                queueCondition_.notify_one();
                
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
            return enabled_.load();
        }

        /**
         * @brief Enable or disable the logger
         */
        void setEnabled(bool enabled) noexcept {
            enabled_.store(enabled);
        }

        /**
         * @brief Get minimum log level
         */
        LogLevel getMinLevel() const noexcept {
            return minLevel_.load();
        }

        /**
         * @brief Set minimum log level
         */
        void setMinLevel(LogLevel level) noexcept {
            minLevel_.store(level);
        }

        /**
         * @brief Get logger name
         */
        std::string_view getName() const noexcept {
            return name_;
        }

        /**
         * @brief Check if asynchronous logging is enabled
         */
        bool isAsyncLogging() const noexcept {
            return asyncLogging_;
        }

        /**
         * @brief Get number of sinks
         */
        std::size_t getSinkCount() const {
            std::lock_guard<std::mutex> lock(sinksMutex_);
            return sinks_.size();
        }

    private:
        /**
         * @brief Check if a log level should be processed
         */
        bool shouldLog(LogLevel level) const noexcept {
            return enabled_.load() && level >= minLevel_.load();
        }

        /**
         * @brief Process a log record (sync or async)
         */
        void processLogRecord(LogRecord record) {
            if (asyncLogging_) {
                // Add to async queue
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    logQueue_.push(std::move(record));
                }
                queueCondition_.notify_one();
            } else {
                // Process synchronously
                writeToSinks(record);
            }
        }

        /**
         * @brief Write log record to all sinks
         */
        void writeToSinks(const LogRecord& record) {
            std::lock_guard<std::mutex> lock(sinksMutex_);
            for (auto& sink : sinks_) {
                if (sink && sink->shouldLog(record.level) && sink->isEnabled()) {
                    try {
                        sink->write(record);
                    } catch (...) {
                        // Ignore sink errors to prevent logging from crashing the application
                        // In a medical device, this might need more sophisticated error handling
                    }
                }
            }
        }

        /**
         * @brief Flush all sinks
         */
        void flushSinks() {
            std::lock_guard<std::mutex> lock(sinksMutex_);
            for (auto& sink : sinks_) {
                if (sink) {
                    try {
                        sink->flush();
                    } catch (...) {
                        // Ignore flush errors
                    }
                }
            }
        }

        /**
         * @brief Async logger thread function
         */
        void asyncLoggerThread() {
            while (!shutdown_.load()) {
                std::unique_lock<std::mutex> lock(queueMutex_);
                
                // Wait for log records or flush requests
                queueCondition_.wait(lock, [this] {
                    return !logQueue_.empty() || !flushPromises_.empty() || shutdown_.load();
                });

                // Process all queued log records
                while (!logQueue_.empty()) {
                    auto record = std::move(logQueue_.front());
                    logQueue_.pop();
                    lock.unlock();
                    
                    writeToSinks(record);
                    
                    lock.lock();
                }

                // Process flush requests
                while (!flushPromises_.empty()) {
                    auto promise = std::move(flushPromises_.front());
                    flushPromises_.pop();
                    lock.unlock();
                    
                    flushSinks();
                    promise.set_value();
                    
                    lock.lock();
                }
            }

            // Process remaining items before shutdown
            std::lock_guard<std::mutex> lock(queueMutex_);
            while (!logQueue_.empty()) {
                writeToSinks(logQueue_.front());
                logQueue_.pop();
            }
            flushSinks();
        }

        /**
         * @brief Shutdown the logger
         */
        void shutdown() {
            if (shutdown_.exchange(true)) return;

            if (asyncThread_.joinable()) {
                queueCondition_.notify_all();
                asyncThread_.join();
            }

            // Final flush
            flushSinks();
        }

        std::string name_;                              ///< Logger name
        bool asyncLogging_;                             ///< Async logging enabled
        std::atomic<bool> enabled_;                     ///< Logger enabled state
        std::atomic<LogLevel> minLevel_;                ///< Minimum log level
        std::atomic<bool> shutdown_;                    ///< Shutdown flag

        // Sinks management
        mutable std::mutex sinksMutex_;                 ///< Sinks container mutex
        SinkContainer sinks_;                           ///< Collection of sinks

        // Async logging
        std::thread asyncThread_;                       ///< Async logging thread
        std::mutex queueMutex_;                         ///< Queue mutex
        std::condition_variable queueCondition_;        ///< Queue condition variable
        std::queue<LogRecord> logQueue_;                ///< Log record queue
        std::queue<std::promise<void>> flushPromises_;  ///< Flush promises queue
    };

} // namespace mddlog::core