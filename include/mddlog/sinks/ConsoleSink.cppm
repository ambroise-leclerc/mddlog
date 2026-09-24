/**
 * @brief Console sink for medical device logging - C++23 Module
 */

export module mddlog.sinks.console;

import std;
import mddlog.core.loglevel;
import mddlog.adapter.logrecord;
import mddlog.sinks.sink;

export namespace mddlog::sinks {

/**
 * @brief Get the color code for console output (ANSI escape sequences)
 * @param level The log level
 * @return ANSI color code string
 *
 * Moved from mddlog.core.loglevel (#32): ANSI styling is presentation, not governed logic, and
 * this sink is its only caller - see ADR-001 Decision 6.
 */
constexpr std::string_view getColorCode(core::LogLevel level) noexcept {
    switch (level) {
        case core::LogLevel::Trace:
            return "\033[37m";    // White
        case core::LogLevel::Debug:
            return "\033[36m";    // Cyan
        case core::LogLevel::Info:
            return "\033[32m";    // Green
        case core::LogLevel::Warn:
            return "\033[33m";    // Yellow
        case core::LogLevel::Error:
            return "\033[31m";    // Red
        case core::LogLevel::Fatal:
            return "\033[35m";    // Magenta
        case core::LogLevel::Audit:
            return "\033[1;34m";  // Bold Blue
        default:
            return "\033[0m";     // Reset
    }
}

/**
 * @brief Reset color code for console output
 */
constexpr std::string_view getResetColorCode() noexcept {
    return "\033[0m";
}

/**
 * @brief Console sink that outputs log records to stdout/stderr
 *
 * This sink provides colored output for different log levels and
 * is thread-safe for concurrent logging operations.
 */
class ConsoleSink : public Sink {
public:
    /**
     * @brief Constructor
     * @param enableColors Enable colored output (default: true)
     * @param enableStderr Use stderr for Error and Fatal levels (default: true)
     */
    explicit ConsoleSink(bool enableColors = true, bool enableStderr = true) : Sink(core::LogLevel::Info), useColors(enableColors), useStderr(enableStderr) {}

    /**
     * @brief Destructor - ensures final flush
     */
    ~ConsoleSink() override {
        flush();
    }

    ConsoleSink(const ConsoleSink&)            = delete;
    ConsoleSink& operator=(const ConsoleSink&) = delete;
    ConsoleSink(ConsoleSink&&)                 = delete;
    ConsoleSink& operator=(ConsoleSink&&)      = delete;

    /**
     * @brief Write a log record to console
     * @param record The log record to write
     */
    void write(const core::LogRecord& record) override {
        if (!shouldLog(record.level) || !isEnabled()) {
            recordDropped();
            return;
        }

        auto start = std::chrono::high_resolution_clock::now();

        std::scoped_lock lock(mutex);

        try {
            // Choose output stream based on log level
            std::ostream& stream = (useStderr && (record.level >= core::LogLevel::Error)) ? std::cerr : std::cout;

            // Add color if enabled
            if (useColors) {
                stream << getColorCode(record.level);
            }

            // Format: [TIMESTAMP] [LEVEL] [CATEGORY] MESSAGE
            stream << "[" << record.getFormattedTimestamp() << "] "
                   << "[" << core::toString(record.level) << "] "
                   << "[" << record.category << "] " << record.message;

            // Add medical device context if available
            if (!record.userId.empty()) {
                stream << " [User:" << record.userId << "]";
            }
            if (!record.deviceId.empty()) {
                stream << " [Device:" << record.deviceId << "]";
            }
            if (!record.operationId.empty()) {
                stream << " [Op:" << record.operationId << "]";
            }

            // Add audit information if present
            if (!record.auditEventType.empty()) {
                stream << " [Audit:" << record.auditEventType << "]";
            }
            if (!record.riskLevel.empty()) {
                stream << " [Risk:" << record.riskLevel << "]";
            }

// Add thread information in debug builds
#ifdef MDDLOG_DEBUG
            stream << " [Thread:" << record.threadId << "]";
#endif

            // Reset color if enabled
            if (useColors) {
                stream << getResetColorCode();
            }

            stream << "\n";

            // Update statistics
            auto end       = std::chrono::high_resolution_clock::now();
            auto writeTime = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

            // Estimate bytes written (rough approximation)
            std::size_t bytesWritten = record.message.length() + record.category.length() + estimatedFormattingOverheadBytes;
            updateStatistics(bytesWritten, writeTime);

        } catch (const std::exception&) {
            // Record failure but don't throw - logging should not crash the application
            recordDropped();
        }
    }

    /**
     * @brief Flush the console output
     */
    void flush() override {
        std::scoped_lock lock(mutex);
        std::cout.flush();
        std::cerr.flush();
        recordFlush();
    }

    /**
     * @brief Get the sink name
     * @return "console"
     */
    std::string_view getName() const noexcept override {
        return "console";
    }

    /**
     * @brief Check if colors are enabled
     * @return True if colored output is enabled
     */
    bool isColorEnabled() const noexcept {
        std::scoped_lock lock(mutex);
        return useColors;
    }

    /**
     * @brief Enable or disable colored output
     * @param value New color state
     */
    void setColorEnabled(bool value) noexcept {
        std::scoped_lock lock(mutex);
        useColors = value;
    }

    /**
     * @brief Check if stderr is used for errors
     * @return True if stderr is used for Error and Fatal levels
     */
    bool isStderrEnabled() const noexcept {
        std::scoped_lock lock(mutex);
        return useStderr;
    }

    /**
     * @brief Enable or disable stderr for error levels
     * @param value New stderr state
     */
    void setStderrEnabled(bool value) noexcept {
        std::scoped_lock lock(mutex);
        useStderr = value;
    }

private:
    /// Rough per-record allowance for the timestamp, level and bracket decorations.
    static constexpr std::size_t estimatedFormattingOverheadBytes = 50;

    mutable std::mutex mutex;      ///< Thread synchronization
    bool               useColors;  ///< Enable colored output
    bool               useStderr;  ///< Use stderr for errors
};

/**
 * @brief Factory function to create a console sink
 * @param useColors Enable colored output
 * @param useStderr Use stderr for error levels
 * @return Shared pointer to console sink
 */
SinkPtr createConsoleSink(bool useColors = true, bool useStderr = true) {
    return std::make_shared<ConsoleSink>(useColors, useStderr);
}

}  // namespace mddlog::sinks