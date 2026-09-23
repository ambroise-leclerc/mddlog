/**
 * @brief Log record structure for medical device logging - C++23 Module
 */

export module mddlog.core.logrecord;

import std;
import mddlog.core.loglevel;

export namespace mddlog::core {

/**
 * @brief Structured log record containing all logging information
 *
 * This structure contains all information needed for medical device
 * compliance logging, including audit trail requirements.
 */
struct LogRecord {
    using TimePoint = std::chrono::system_clock::time_point;
    using ThreadId  = std::thread::id;
    using Metadata  = std::unordered_map<std::string, std::string>;

    // Core logging information
    TimePoint   timestamp;              ///< When the log was created
    LogLevel    level{LogLevel::Info};  ///< Severity level
    std::string message;                ///< Log message content
    std::string category;               ///< Log category/component
    ThreadId    threadId;               ///< Thread that created the log

    // Source location information (C++20 feature)
    std::source_location location;  ///< Source code location

    // Medical device compliance fields
    std::string userId;       ///< User who triggered the action
    std::string sessionId;    ///< Session identifier
    std::string deviceId;     ///< Medical device identifier
    std::string operationId;  ///< Operation/procedure identifier

    // Audit trail fields
    std::string auditEventType;      ///< Type of audit event
    std::string riskLevel;           ///< Associated risk level
    std::string complianceStandard;  ///< Applicable compliance standard

    // Additional metadata
    Metadata metadata;  ///< Custom key-value pairs

    // Performance tracking
    std::chrono::nanoseconds processingTime{0};  ///< Time to process the log

    /**
     * @brief Default constructor
     */
    LogRecord() = default;

    /**
     * @brief Constructor for basic logging
     * @param lvl Log severity level
     * @param msg Log message
     * @param cat Log category
     * @param loc Source location (auto-filled)
     */
    LogRecord(LogLevel lvl, std::string_view msg, std::string_view cat = "default", const std::source_location& loc = std::source_location::current())
        : timestamp(std::chrono::system_clock::now()), level(lvl), message(msg), category(cat), threadId(std::this_thread::get_id()), location(loc) {}

    /**
     * @brief Constructor for medical compliance logging
     * @param lvl Log severity level
     * @param msg Log message
     * @param cat Log category
     * @param uid User identifier
     * @param sid Session identifier
     * @param did Device identifier
     * @param loc Source location (auto-filled)
     */
    LogRecord(LogLevel                    lvl,
              std::string_view            msg,
              std::string_view            cat,
              std::string_view            uid,
              std::string_view            sid,
              std::string_view            did,
              const std::source_location& loc = std::source_location::current())
        : timestamp(std::chrono::system_clock::now()),
          level(lvl),
          message(msg),
          category(cat),
          threadId(std::this_thread::get_id()),
          location(loc),
          userId(uid),
          sessionId(sid),
          deviceId(did) {}

    /**
     * @brief Set audit information
     * @param eventType Type of audit event
     * @param riskLvl Risk level assessment
     * @param standard Compliance standard reference
     */
    void setAuditInfo(std::string_view eventType, std::string_view riskLvl = "", std::string_view standard = "IEC_62304") {
        auditEventType     = eventType;
        riskLevel          = riskLvl;
        complianceStandard = standard;
    }

    /**
     * @brief Add custom metadata
     * @param key Metadata key
     * @param value Metadata value
     */
    void addMetadata(std::string_view key, std::string_view value) {
        metadata[std::string(key)] = std::string(value);
    }

    /**
     * @brief Check if this record requires compliance logging
     * @return True if compliance features should be applied
     */
    [[nodiscard]] bool isComplianceRequired() const noexcept {
        return isComplianceLevel(level) || !userId.empty() || !auditEventType.empty();
    }

    /**
     * @brief Get formatted timestamp string
     * @return ISO 8601 UTC timestamp with millisecond precision, e.g. "2026-09-22T07:15:57.160Z"
     *
     * Formats via std::chrono's formatter rather than std::gmtime(): gmtime() returns a
     * pointer into a static buffer that is not thread-safe, and MSVC additionally deprecates
     * it (C4996), which fails this project's warnings-as-errors build.
     */
    [[nodiscard]] std::string getFormattedTimestamp() const {
        using namespace std::chrono;
        return std::format("{:%Y-%m-%dT%H:%M:%S}Z", time_point_cast<milliseconds>(timestamp));
    }

    /**
     * @brief Get source location as string
     * @return Formatted source location
     */
    [[nodiscard]] std::string getSourceLocationString() const {
        std::stringstream ss;
        ss << location.file_name() << ':' << location.line() << ':' << location.column() << " in " << location.function_name();
        return ss.str();
    }
};

/**
 * @brief Statistics for tracking logging performance
 */
struct LogStatistics {
    std::atomic<std::uint64_t> recordsWritten{0};    ///< Total records written
    std::atomic<std::uint64_t> recordsDropped{0};    ///< Records dropped due to errors
    std::atomic<std::uint64_t> bytesWritten{0};      ///< Total bytes written
    std::atomic<std::uint64_t> flushCount{0};        ///< Number of flush operations
    std::atomic<std::uint64_t> totalWriteTimeNs{0};  ///< Total write time in nanoseconds

    // Make non-copyable because of atomic members
    LogStatistics()                                = default;
    ~LogStatistics()                               = default;
    LogStatistics(const LogStatistics&)            = delete;
    LogStatistics& operator=(const LogStatistics&) = delete;
    LogStatistics(LogStatistics&&)                 = delete;
    LogStatistics& operator=(LogStatistics&&)      = delete;

    /**
     * @brief Reset all statistics
     */
    void reset() noexcept {
        recordsWritten.store(0);
        recordsDropped.store(0);
        bytesWritten.store(0);
        flushCount.store(0);
        totalWriteTimeNs.store(0);
    }

    /**
     * @brief Get average write time per record
     * @return Average time in nanoseconds
     */
    std::chrono::nanoseconds getAverageWriteTime() const noexcept {
        auto records = recordsWritten.load();
        if (records == 0)
            return std::chrono::nanoseconds{0};
        return std::chrono::nanoseconds{totalWriteTimeNs.load() / records};
    }

    /**
     * @brief Get total write time
     * @return Total write time in nanoseconds
     */
    std::chrono::nanoseconds getTotalWriteTime() const noexcept {
        return std::chrono::nanoseconds{totalWriteTimeNs.load()};
    }
};

}  // namespace mddlog::core