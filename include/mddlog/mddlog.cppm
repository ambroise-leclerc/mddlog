/**
 * @brief MddLog - Modern C++23 Medical Device Logger
 *
 * A C++23 module library with diagnostic logging and bounded in-memory audit admission.
 *
 * Features:
 * - Thread-safe logging with C++23 features
 * - Diagnostic console sinks and separate audit sink interface
 * - Medical device design context, without a certification claim
 * - Audit admission and hand-off without durability or tamper-evidence
 * - Risk management and lifecycle logging
 * - Performance monitoring and metrics
 * - Standard library module support
 * - Cross-platform (MSVC, GCC, Clang)
 */

export module mddlog;

import std;

// Import submodules
import mddlog.core.loglevel;
import mddlog.core.auditring;
import mddlog.adapter.logrecord;
import mddlog.adapter.ringdrain;
import mddlog.adapter.auditdrain;
import mddlog.adapter.auditchain;
import mddlog.adapter.auditanchor;
import mddlog.adapter.auditverifier;
import mddlog.adapter.auditlayout;
import mddlog.adapter.auditmedium;
import mddlog.adapter.auditledger;
import mddlog.adapter.auditlog;
import mddlog.adapter.auditlogverifier;
import mddlog.adapter.auditstore;
import mddlog.adapter.logger;
import mddlog.sinks.sink;
import mddlog.sinks.auditsink;
import mddlog.sinks.console;

// Re-export for convenience
export namespace mddlog {
// NOLINTBEGIN(misc-unused-using-decls): these using-declarations ARE the module's exported API; nothing
// in this interface unit uses them, only importers do.
// Re-export core types
using adapter::AdvanceAnswer;
using adapter::AdvanceRefusal;
using adapter::AgeStatus;
using adapter::Anchor;
using adapter::AnchorAbsent;
using adapter::AnchorClaim;
using adapter::AnchorProvider;
using adapter::AnchorStamp;
using adapter::AnchorVerifier;
using adapter::AppendAnswer;
using adapter::AppendStatus;
using adapter::AuditChain;
using adapter::AuditChainVerifier;
using adapter::AuditDrainResult;
using adapter::AuditDrainStatus;
using adapter::AuditHealthSnapshot;
using adapter::AuditRingRegistration;
using adapter::AuditSinkAdapter;
using adapter::BoundaryKind;
using adapter::boundaryName;
using adapter::BoundaryNote;
using adapter::buildLedgerEvent;
using adapter::canonicalContractVersion;
using adapter::CanonicalRecord;
using adapter::ChainedRecord;
using adapter::ChainFinding;
using adapter::ChainRefusal;
using adapter::ChainStateRecovery;
using adapter::checkMediumAtStart;
using adapter::CitedPosition;
using adapter::Continuity;
using adapter::crc32c;
using adapter::digestFromHex;
using adapter::encodeCanonical;
using adapter::encodeRecordFrame;
using adapter::encodeSegmentOpening;
using adapter::InMemoryAnchorProvider;
using adapter::InMemoryStorageMedium;
using adapter::IntegrityFault;
using adapter::IntegrityFaultKind;
using adapter::isLedgerHeadAction;
using adapter::LatestAnswer;
using adapter::ledgerActionName;
using adapter::LedgerCheck;
using adapter::LedgerChecker;
using adapter::LedgerConfig;
using adapter::LedgerEntry;
using adapter::LedgerFault;
using adapter::LedgerImage;
using adapter::ledgerKindOf;
using adapter::LedgerRecordKind;
using adapter::LedgerRecordView;
using adapter::ledgerReserveSegments;
using adapter::LogAnalysis;
using adapter::LogImage;
using adapter::LogReport;
using adapter::LogVerifier;
using adapter::makeAnchorClaim;
using adapter::makeBoundaryNote;
using adapter::OpenAnswer;
using adapter::OpenStatus;
using adapter::PendingRetirement;
using adapter::PersistingAuditSink;
using adapter::ProviderListing;
using adapter::ProviderUnavailable;
using adapter::readStoredStream;
using adapter::recoverChainState;
using adapter::RecoveredStream;
using adapter::RecoveryFinding;
using adapter::RecoveryFindingKind;
using adapter::RecoveryReport;
using adapter::RestartReport;
using adapter::RetainedAnchor;
using adapter::RetainedOutcome;
using adapter::RetainedPosition;
using adapter::RetentionOutcome;
using adapter::RetentionPolicy;
using adapter::RetentionResult;
using adapter::RetireAnswer;
using adapter::Retirement;
using adapter::RetireRefusal;
using adapter::RingSinkAdapter;
using adapter::ScannedRecord;
using adapter::scanSegment;
using adapter::SegmentHeader;
using adapter::SegmentImage;
using adapter::SegmentInfo;
using adapter::SegmentOpening;
using adapter::SegmentRef;
using adapter::SegmentScan;
using adapter::SegmentStatus;
using adapter::Sha256Digest;
using adapter::StorageConfig;
using adapter::StorageConfigError;
using adapter::StorageCounters;
using adapter::StorageHealthSnapshot;
using adapter::StorageIssue;
using adapter::storageLayoutVersion;
using adapter::StorageMedium;
using adapter::StoredRecord;
using adapter::StoredStream;
using adapter::StreamBoundaryReport;
using adapter::StreamDisposition;
using adapter::StreamEntry;
using adapter::StreamEvaluation;
using adapter::StreamImage;
using adapter::StreamReport;
using adapter::StreamsAnswer;
using adapter::StreamStart;
using adapter::StreamStorageHealth;
using adapter::StreamStorageState;
using adapter::SyncAnswer;
using adapter::SyncPolicy;
using adapter::TrailingRegion;
using adapter::TrimRecord;
using adapter::Verdict;
using adapter::VerdictCause;
using adapter::verdictName;
using adapter::VerifierConfig;
using core::AuditCategory;
using core::AuditEvent;
using core::AuditField;
using core::AuditInput;
using core::AuditPhase;
using core::AuditRefusal;
using core::AuditRefusalReason;
using core::auditReservedActionPrefix;
using core::AuditRing;
using core::AuditWriteResult;
using core::fromString;
using core::isComplianceLevel;
using core::isReservedAuditAction;
using core::LogLevel;
using core::LogRecord;
using core::LogStatistics;
using core::RawTime;
using core::SimpleLogger;
using core::toString;

// Re-export sinks
using sinks::AuditSink;
using sinks::AuditSinkPtr;
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
