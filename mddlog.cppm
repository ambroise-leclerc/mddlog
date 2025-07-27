/**
 * @brief Main module interface for mddlog - Medical Device Logger
 * @file mddlog.cppm
 * 
 * This is the primary module interface that exports all mddlog functionality.
 * Compliant with IEC 62304 and ISO 13485 standards for medical device software.
 */

export module mddlog;

import std;

// Re-export the LogLevel module
export import mddlog.core.loglevel;

export namespace mddlog {

    // Forward declarations
    namespace core {
        class LogRecord;
        class SimpleLogger;
    }
    
    namespace sinks {
        class Sink;
        class ConsoleSink;
    }

    // Re-export core types
    using core::LogLevel;
    using core::LogRecord;
    using core::SimpleLogger;
    
    // Re-export sink types
    using sinks::Sink;
    using sinks::ConsoleSink;

} // namespace mddlog

// Include implementation modules
module :private;

// LogRecord implementation
export namespace mddlog::core {

    /**
     * @brief Structured log record for medical device logging
     * 
     * Contains all necessary information for compliance logging including
     * timestamps, source location, user context, and metadata.
     */
    class LogRecord {
    public:
        using Metadata = std::unordered_map<std::string, std::string>;
        
        // Core log data
        LogLevel level;                          ///< Log severity level
        std::string message;                     ///< Log message content
        std::chrono::system_clock::time_point timestamp; ///< When the log was created
        std::source_location location;          ///< Source code location
        
        // Medical device context
        std::string user_id;                     ///< User identifier for audit trail
        std::string session_id;                  ///< Session identifier
        std::string device_id;                   ///< Medical device identifier
        std::string software_version;            ///< Software version for traceability
        
        // Additional metadata
        Metadata metadata;                       ///< Custom key-value pairs
        
        /**
         * @brief Construct a log record with basic information
         */
        LogRecord(LogLevel lvl, 
                 std::string msg,
                 const std::source_location& loc = std::source_location::current())
            : level(lvl)
            , message(std::move(msg))
            , timestamp(std::chrono::system_clock::now())
            , location(loc) {}
        
        /**
         * @brief Construct a log record with full medical device context
         */
        LogRecord(LogLevel lvl,
                 std::string msg,
                 std::string uid,
                 std::string sid,
                 std::string did,
                 std::string version,
                 const std::source_location& loc = std::source_location::current())
            : level(lvl)
            , message(std::move(msg))
            , timestamp(std::chrono::system_clock::now())
            , location(loc)
            , user_id(std::move(uid))
            , session_id(std::move(sid))
            , device_id(std::move(did))
            , software_version(std::move(version)) {}
        
        /**
         * @brief Add metadata key-value pair
         */
        void addMetadata(const std::string& key, const std::string& value) {
            metadata[key] = value;
        }
        
        /**
         * @brief Get metadata value by key
         */
        std::string getMetadata(const std::string& key) const {
            auto it = metadata.find(key);
            return it != metadata.end() ? it->second : std::string{};
        }
        
        /**
         * @brief Format timestamp as ISO 8601 string
         */
        std::string formatTimestamp() const {
            auto time_t = std::chrono::system_clock::to_time_t(timestamp);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                timestamp.time_since_epoch()) % 1000;
            
            std::stringstream ss;
            ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S.");
            ss << std::setfill('0') << std::setw(3) << ms.count() << "Z";
            return ss.str();
        }
        
        /**
         * @brief Format source location as string
         */
        std::string formatLocation() const {
            return std::string(location.file_name()) + ":" +
                   std::to_string(static_cast<unsigned>(location.line())) + ":" +
                   std::to_string(static_cast<unsigned>(location.column())) + " in " +
                   std::string(location.function_name());
        }
        
        /**
         * @brief Check if this record requires compliance logging
         */
        bool requiresCompliance() const noexcept {
            return isComplianceLevel(level);
        }
    };

} // namespace mddlog::core

// Sink interface implementation
export namespace mddlog::sinks {

    /**
     * @brief Base interface for log output destinations
     */
    class Sink {
    public:
        virtual ~Sink() = default;
        
        /**
         * @brief Write a log record to the sink
         */
        virtual void write(const core::LogRecord& record) = 0;
        
        /**
         * @brief Flush any buffered output
         */
        virtual void flush() = 0;
        
        /**
         * @brief Check if the sink should accept records of this level
         */
        virtual bool shouldLog(LogLevel level) const noexcept = 0;
        
        /**
         * @brief Set minimum log level for this sink
         */
        virtual void setLevel(LogLevel level) noexcept = 0;
        
        /**
         * @brief Get current minimum log level
         */
        virtual LogLevel getLevel() const noexcept = 0;
        
        /**
         * @brief Enable or disable the sink
         */
        virtual void setEnabled(bool enabled) noexcept = 0;
        
        /**
         * @brief Check if the sink is enabled
         */
        virtual bool isEnabled() const noexcept = 0;
    };

    /**
     * @brief Console output sink with color support
     */
    class ConsoleSink : public Sink {
    private:
        LogLevel min_level_ = LogLevel::INFO;
        bool enabled_ = true;
        bool use_colors_ = true;
        mutable std::mutex mutex_;

    public:
        ConsoleSink() = default;
        explicit ConsoleSink(LogLevel level) : min_level_(level) {}
        
        void write(const core::LogRecord& record) override {
            if (!shouldLog(record.level)) return;
            
            std::lock_guard<std::mutex> lock(mutex_);
            
            if (use_colors_) {
                std::cout << getColorCode(record.level);
            }
            
            std::cout << "[" << record.formatTimestamp() << "] "
                      << "[" << toString(record.level) << "] "
                      << record.message;
            
            if (!record.user_id.empty()) {
                std::cout << " [User: " << record.user_id << "]";
            }
            
            if (!record.session_id.empty()) {
                std::cout << " [Session: " << record.session_id << "]";
            }
            
            if (!record.device_id.empty()) {
                std::cout << " [Device: " << record.device_id << "]";
            }
            
            std::cout << " (" << record.formatLocation() << ")";
            
            if (use_colors_) {
                std::cout << getResetColorCode();
            }
            
            std::cout << std::endl;
        }
        
        void flush() override {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout.flush();
        }
        
        bool shouldLog(LogLevel level) const noexcept override {
            return enabled_ && level >= min_level_;
        }
        
        void setLevel(LogLevel level) noexcept override {
            std::lock_guard<std::mutex> lock(mutex_);
            min_level_ = level;
        }
        
        LogLevel getLevel() const noexcept override {
            std::lock_guard<std::mutex> lock(mutex_);
            return min_level_;
        }
        
        void setEnabled(bool enabled) noexcept override {
            std::lock_guard<std::mutex> lock(mutex_);
            enabled_ = enabled;
        }
        
        bool isEnabled() const noexcept override {
            std::lock_guard<std::mutex> lock(mutex_);
            return enabled_;
        }
        
        void setUseColors(bool use_colors) noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            use_colors_ = use_colors;
        }
        
        bool getUseColors() const noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            return use_colors_;
        }
    };

} // namespace mddlog::sinks

// Simple Logger implementation
export namespace mddlog::core {

    /**
     * @brief Thread-safe logger with multiple sinks support
     */
    class SimpleLogger {
    private:
        std::vector<std::unique_ptr<sinks::Sink>> sinks_;
        LogLevel min_level_ = LogLevel::INFO;
        std::string default_user_id_;
        std::string default_session_id_;
        std::string default_device_id_;
        std::string software_version_;
        mutable std::mutex mutex_;
        
    public:
        SimpleLogger() = default;
        
        /**
         * @brief Add a sink to the logger
         */
        void addSink(std::unique_ptr<sinks::Sink> sink) {
            std::lock_guard<std::mutex> lock(mutex_);
            sinks_.push_back(std::move(sink));
        }
        
        /**
         * @brief Set default medical device context
         */
        void setMedicalContext(const std::string& user_id,
                              const std::string& session_id,
                              const std::string& device_id,
                              const std::string& version) {
            std::lock_guard<std::mutex> lock(mutex_);
            default_user_id_ = user_id;
            default_session_id_ = session_id;
            default_device_id_ = device_id;
            software_version_ = version;
        }
        
        /**
         * @brief Log a message with specified level
         */
        void log(LogLevel level, const std::string& message,
                const std::source_location& loc = std::source_location::current()) {
            if (level < min_level_) return;
            
            LogRecord record(level, message, default_user_id_, default_session_id_,
                           default_device_id_, software_version_, loc);
            
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& sink : sinks_) {
                if (sink && sink->shouldLog(level)) {
                    sink->write(record);
                }
            }
        }
        
        /**
         * @brief Convenience methods for different log levels
         */
        void trace(const std::string& message, const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::TRACE, message, loc);
        }
        
        void debug(const std::string& message, const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::DEBUG, message, loc);
        }
        
        void info(const std::string& message, const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::INFO, message, loc);
        }
        
        void warn(const std::string& message, const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::WARN, message, loc);
        }
        
        void error(const std::string& message, const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::ERROR, message, loc);
        }
        
        void fatal(const std::string& message, const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::FATAL, message, loc);
        }
        
        void audit(const std::string& message, const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::AUDIT, message, loc);
        }
        
        /**
         * @brief Flush all sinks
         */
        void flush() {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& sink : sinks_) {
                if (sink) {
                    sink->flush();
                }
            }
        }
        
        /**
         * @brief Set minimum log level
         */
        void setLevel(LogLevel level) noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            min_level_ = level;
        }
        
        /**
         * @brief Get current minimum log level
         */
        LogLevel getLevel() const noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            return min_level_;
        }
    };

} // namespace mddlog::core