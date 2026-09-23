/**
 * @brief MddLog - Modern C++23 Medical Device Logger
 *
 * A header-only logging library designed for medical devices,
 * conformant to IEC 62304 and ISO 13485 standards.
 *
 * Features:
 * - Thread-safe logging with C++23 features
 * - Multiple sink support (file, console, network, audit)
 * - Medical device compliance logging
 * - Audit trail with tamper-proof records
 * - Risk management and lifecycle logging
 * - Performance monitoring and metrics
 * - Zero-dependency (import std only)
 * - Cross-platform (MSVC, GCC, Clang)
 */

export module mddlog;

import std;

// Import submodules
import mddlog.core.loglevel;
import mddlog.core.logrecord;
import mddlog.core.logger;
import mddlog.sinks.sink;
import mddlog.sinks.console;

// Re-export for convenience
export namespace mddlog {
// Re-export core types
using core::fromString;
using core::getColorCode;
using core::getResetColorCode;
using core::isComplianceLevel;
using core::LogLevel;
using core::LogRecord;
using core::LogStatistics;
using core::SimpleLogger;
using core::toString;

// Re-export sinks
using sinks::ConsoleSink;
using sinks::createConsoleSink;
using sinks::Sink;
using sinks::SinkPtr;
using sinks::WeakSinkPtr;

/**
 * @brief Get the library version
 * @return Version string in format "major.minor.patch"
 */
constexpr std::string_view getVersion() noexcept {
    return "0.1.0";
}

/**
 * @brief Check if medical device compliance is enabled
 * @return True if compliance features are active
 */
constexpr bool isMedicalComplianceEnabled() noexcept {
#ifdef MDDLOG_MEDICAL_DEVICE_COMPLIANCE
    return true;
#else
    return false;
#endif
}
}  // namespace mddlog