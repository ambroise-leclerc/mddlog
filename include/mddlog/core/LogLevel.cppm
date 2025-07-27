/**
 * @brief Log severity levels for medical device logging - C++23 Module
 */

export module mddlog.core.loglevel;

import std;

export namespace mddlog::core {

    /**
     * @brief Enumeration of log severity levels
     * 
     * Ordered from lowest to highest severity for filtering purposes.
     * Includes medical device specific levels for compliance.
     */
    enum class LogLevel : std::uint8_t {
        TRACE = 0,      ///< Detailed trace information for debugging
        DEBUG = 1,      ///< Debug information for development
        INFO = 2,       ///< General information messages
        WARN = 3,       ///< Warning conditions that should be noted
        ERROR = 4,      ///< Error conditions that affect functionality
        FATAL = 5,      ///< Fatal errors that may cause system failure
        AUDIT = 6       ///< Audit trail entries for compliance (highest priority)
    };

    /**
     * @brief Convert log level to string representation
     * @param level The log level to convert
     * @return String representation of the log level
     */
    constexpr std::string_view toString(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            case LogLevel::AUDIT: return "AUDIT";
            default: return "UNKNOWN";
        }
    }

    /**
     * @brief Convert string to log level
     * @param str String representation of log level
     * @return Corresponding LogLevel, or LogLevel::INFO if not found
     */
    constexpr LogLevel fromString(std::string_view str) noexcept {
        if (str == "TRACE") return LogLevel::TRACE;
        if (str == "DEBUG") return LogLevel::DEBUG;
        if (str == "INFO")  return LogLevel::INFO;
        if (str == "WARN")  return LogLevel::WARN;
        if (str == "ERROR") return LogLevel::ERROR;
        if (str == "FATAL") return LogLevel::FATAL;
        if (str == "AUDIT") return LogLevel::AUDIT;
        return LogLevel::INFO; // Default fallback
    }

    /**
     * @brief Check if a log level is enabled for medical compliance
     * @param level The log level to check
     * @return True if the level requires compliance logging
     */
    constexpr bool isComplianceLevel(LogLevel level) noexcept {
        return level >= LogLevel::WARN; // WARN, ERROR, FATAL, AUDIT require compliance
    }

    /**
     * @brief Get the color code for console output (ANSI escape sequences)
     * @param level The log level
     * @return ANSI color code string
     */
    constexpr std::string_view getColorCode(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::TRACE: return "\033[37m";    // White
            case LogLevel::DEBUG: return "\033[36m";    // Cyan
            case LogLevel::INFO:  return "\033[32m";    // Green
            case LogLevel::WARN:  return "\033[33m";    // Yellow
            case LogLevel::ERROR: return "\033[31m";    // Red
            case LogLevel::FATAL: return "\033[35m";    // Magenta
            case LogLevel::AUDIT: return "\033[1;34m";  // Bold Blue
            default: return "\033[0m";                  // Reset
        }
    }

    /**
     * @brief Reset color code for console output
     */
    constexpr std::string_view getResetColorCode() noexcept {
        return "\033[0m";
    }

} // namespace mddlog::core