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
    Trace = 0,  ///< Detailed trace information for debugging
    Debug = 1,  ///< Debug information for development
    Info  = 2,  ///< General information messages
    Warn  = 3,  ///< Warning conditions that should be noted
    Error = 4,  ///< Error conditions that affect functionality
    Fatal = 5,  ///< Fatal errors that may cause system failure
    Audit = 6   ///< Audit trail entries for compliance (highest priority)
};

/**
 * @brief Convert log level to string representation
 * @param level The log level to convert
 * @return String representation of the log level
 */
constexpr std::string_view toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
        case LogLevel::Audit:
            return "AUDIT";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Convert string to log level
 * @param str String representation of log level
 * @return Corresponding LogLevel, or LogLevel::Info if not found
 */
constexpr LogLevel fromString(std::string_view str) noexcept {
    if (str == "TRACE")
        return LogLevel::Trace;
    if (str == "DEBUG")
        return LogLevel::Debug;
    if (str == "INFO")
        return LogLevel::Info;
    if (str == "WARN")
        return LogLevel::Warn;
    if (str == "ERROR")
        return LogLevel::Error;
    if (str == "FATAL")
        return LogLevel::Fatal;
    if (str == "AUDIT")
        return LogLevel::Audit;
    return LogLevel::Info;  // Default fallback
}

/**
 * @brief Check if a log level is enabled for medical compliance
 * @param level The log level to check
 * @return True if the level requires compliance logging
 */
constexpr bool isComplianceLevel(LogLevel level) noexcept {
    return level >= LogLevel::Warn;  // Warn, Error, Fatal, Audit require compliance
}

/**
 * @brief Get the color code for console output (ANSI escape sequences)
 * @param level The log level
 * @return ANSI color code string
 */
constexpr std::string_view getColorCode(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "\033[37m";    // White
        case LogLevel::Debug:
            return "\033[36m";    // Cyan
        case LogLevel::Info:
            return "\033[32m";    // Green
        case LogLevel::Warn:
            return "\033[33m";    // Yellow
        case LogLevel::Error:
            return "\033[31m";    // Red
        case LogLevel::Fatal:
            return "\033[35m";    // Magenta
        case LogLevel::Audit:
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

}  // namespace mddlog::core