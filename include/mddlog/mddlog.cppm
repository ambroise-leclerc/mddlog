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
import mddlog.adapter.logrecord;
import mddlog.adapter.ringdrain;
import mddlog.adapter.logger;
import mddlog.sinks.sink;
import mddlog.sinks.console;

// Re-export for convenience
export namespace mddlog {
// NOLINTBEGIN(misc-unused-using-decls): these using-declarations ARE the module's exported API; nothing
// in this interface unit uses them, only importers do.
// Re-export core types
using adapter::RingSinkAdapter;
using core::fromString;
using core::isComplianceLevel;
using core::LogLevel;
using core::LogRecord;
using core::LogStatistics;
using core::SimpleLogger;
using core::toString;

// Re-export sinks
using sinks::ConsoleSink;
using sinks::createConsoleSink;
using sinks::getColorCode;
using sinks::getResetColorCode;
using sinks::Sink;
using sinks::SinkPtr;
using sinks::WeakSinkPtr;
// NOLINTEND(misc-unused-using-decls)

/**
 * @brief Get the library version
 * @return Version string in format "major.minor.patch"
 */
constexpr std::string_view getVersion() noexcept {
    // Derived from project(mddlog VERSION ...) through mddlog_options; never repeat the literal here.
    return MDDLOG_VERSION_STRING;
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
