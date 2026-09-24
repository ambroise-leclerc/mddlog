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

}  // namespace mddlog::core

// getColorCode()/getResetColorCode() (ANSI console styling) moved to mddlog.sinks.console (#32):
// ANSI escape sequences are a sink-zone presentation concern, not governed logic, and
// ConsoleSink::write() was their only caller - see ADR-001 Decision 6.