/** @brief Byte-preserving JSON projection and explicit audit-tool exit policy (#119). */
export module mddlog.adapter.auditprojection;
import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditevidence;

export namespace mddlog::adapter {
inline constexpr std::uint64_t auditProjectionVersion = 1;
enum class AuditToolExit : std::uint8_t { Covered = 0, Impossible = 2, Adverse = 3, Partial = 4 };

/** @brief Operation failures take precedence; adverse findings take precedence over coverage reservations. */
[[nodiscard]] inline AuditToolExit auditToolExit(const LogReport& report, const LogImage& image) {
    if (report.mediumUnreadable || report.resourceIssue != AuditResourceIssue::None || image.resourceIssue() != AuditResourceIssue::None || image.unreadable()
        || image.unknownLayoutVersion())
        return AuditToolExit::Impossible;
    bool       adverse = false;
    bool       partial = report.streams.empty() || image.segmentsWithoutHeader() != 0;
    const auto check   = [&](const StreamReport& stream) {
        switch (stream.verdict) {
            case Verdict::CannotVerify:
                return AuditToolExit::Impossible;
            case Verdict::Inconsistent:
            case Verdict::RolledBack:
            case Verdict::Conflict:
            case Verdict::Altered:
            case Verdict::Incomplete:
                return AuditToolExit::Adverse;
            case Verdict::Unanchored:
            case Verdict::AnchorUnavailable:
                return AuditToolExit::Partial;
            case Verdict::Anchored:
            case Verdict::Retired:
                return stream.unanchoredFrom || stream.age == AgeStatus::Stale || stream.age == AgeStatus::Unknown ? AuditToolExit::Partial
                                                                                                                     : AuditToolExit::Covered;
        }
        return AuditToolExit::Impossible;
    };
    for (const auto& stream : report.streams) {
        const auto result = check(stream.report);
        if (result == AuditToolExit::Impossible)
            return result;
        adverse = adverse || result == AuditToolExit::Adverse;
        partial = partial || result == AuditToolExit::Partial || stream.trailing.has_value();
    }
    for (const auto& stream : report.unlisted) {
        const auto result = check(stream);
        if (result == AuditToolExit::Impossible)
            return result;
        adverse = adverse || result == AuditToolExit::Adverse;
        partial = partial || result == AuditToolExit::Partial;
    }
    for (const auto& note : report.notes) {
        switch (note.kind) {
            case BoundaryKind::PredecessorNotReproduced:
            case BoundaryKind::PredecessorFailedForm:
            case BoundaryKind::RecoveredNotReproduced:
            case BoundaryKind::RecoveredFailedForm:
            case BoundaryKind::ClosedCitationNotReproduced:
            case BoundaryKind::PrefixMissingWithoutTrim:
            case BoundaryKind::RecordsMissingAfterTrim:
            case BoundaryKind::TrimPastAnchor:
            case BoundaryKind::OriginClaimedWhileHistoryExists:
            case BoundaryKind::LedgersFork:
            case BoundaryKind::LedgerCitationCycle:
            case BoundaryKind::StreamNotOpenedByAnyLedger:
            case BoundaryKind::ReservedActionOutsideLedger:
            case BoundaryKind::LedgerRecordMalformed:
                adverse = true;
                break;
            case BoundaryKind::PredecessorNotCheckable:
            case BoundaryKind::RecoveredNotCheckable:
            case BoundaryKind::EndedWithoutClose:
            case BoundaryKind::NoCloseRecorded:
            case BoundaryKind::LedgerNotClosed:
            case BoundaryKind::RemovedNeverAnchored:
            case BoundaryKind::RemovedWithoutAnchor:
            case BoundaryKind::LeftoverFromInterruptedRemoval:
            case BoundaryKind::RemovalNotCarriedOut:
                partial = true;
                break;
            default:
                break;
        }
    }
    if (adverse)
        return AuditToolExit::Adverse;
    return partial ? AuditToolExit::Partial : AuditToolExit::Covered;
}

namespace projection {
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers): return literals are the frozen projection v1 IDs in
// docs/audit-export-format.md.
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(Verdict value) {
    switch (value) {
        case Verdict::Inconsistent:
            return 0;
        case Verdict::CannotVerify:
            return 1;
        case Verdict::RolledBack:
            return 2;
        case Verdict::Conflict:
            return 3;
        case Verdict::Altered:
            return 4;
        case Verdict::Retired:
            return 5;
        case Verdict::Incomplete:
            return 6;
        case Verdict::Anchored:
            return 7;
        case Verdict::Unanchored:
            return 8;
        case Verdict::AnchorUnavailable:
            return 9;
    }
    throw std::invalid_argument("unknown Verdict in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(VerdictCause value) {
    switch (value) {
        case VerdictCause::None:
            return 0;
        case VerdictCause::InvalidStreamIdentity:
            return 1;
        case VerdictCause::UnsupportedStreamVersion:
            return 2;
        case VerdictCause::UnsupportedLayoutVersion:
            return 3;
        case VerdictCause::StorageUnreadable:
            return 4;
        case VerdictCause::SegmentLayoutInconsistent:
            return 5;
        case VerdictCause::AnchorFormatUnknown:
            return 6;
        case VerdictCause::AnchorMissingField:
            return 7;
        case VerdictCause::TrimPastAnchor:
            return 8;
        case VerdictCause::ProviderHeadBelowRetained:
            return 9;
        case VerdictCause::StreamAnchorBelowRetained:
            return 10;
        case VerdictCause::RetirementUndone:
            return 11;
        case VerdictCause::StreamMissing:
            return 12;
        case VerdictCause::AnchorConflict:
            return 13;
        case VerdictCause::AnchorDigestMismatch:
            return 14;
        case VerdictCause::RetainedCheckpointMismatch:
            return 15;
        case VerdictCause::LogEndsBeforeAnchor:
            return 16;
        case VerdictCause::LogEndsBeforeRetained:
            return 17;
        case VerdictCause::NoAnchor:
            return 18;
        case VerdictCause::ProviderUnavailable:
            return 19;
        case VerdictCause::LedgerRecordMalformed:
            return 20;
        case VerdictCause::TrimDigestMismatch:
            return 21;
        case VerdictCause::LeftoverDoesNotReachTrim:
            return 22;
        case VerdictCause::TrimNotOnSegmentBoundary:
            return 23;
        case VerdictCause::PrefixMissingWithoutTrim:
            return 24;
        case VerdictCause::RecordsMissingAfterTrim:
            return 25;
        case VerdictCause::TrimExceedsRecords:
            return 26;
        case VerdictCause::TrimDiffersFromRetirement:
            return 27;
        case VerdictCause::RetirementBeyondTrim:
            return 28;
        case VerdictCause::ResourceLimit:
            return 29;
    }
    throw std::invalid_argument("unknown VerdictCause in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(ChainFinding value) {
    switch (value) {
        case ChainFinding::Ok:
            return 0;
        case ChainFinding::Malformed:
            return 1;
        case ChainFinding::InvalidVerifier:
            return 2;
        case ChainFinding::VersionChange:
            return 3;
        case ChainFinding::UnsupportedVersion:
            return 4;
        case ChainFinding::StreamMismatch:
            return 5;
        case ChainFinding::SequenceBreak:
            return 6;
        case ChainFinding::DigestMismatch:
            return 7;
    }
    throw std::invalid_argument("unknown ChainFinding in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(AgeStatus value) {
    switch (value) {
        case AgeStatus::NotChecked:
            return 0;
        case AgeStatus::Fresh:
            return 1;
        case AgeStatus::Stale:
            return 2;
        case AgeStatus::Unknown:
            return 3;
    }
    throw std::invalid_argument("unknown AgeStatus in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(RetainedOutcome value) {
    switch (value) {
        case RetainedOutcome::NoneRetained:
            return 0;
        case RetainedOutcome::Passed:
            return 1;
        case RetainedOutcome::RolledBack:
            return 2;
        case RetainedOutcome::NotChecked:
            return 3;
        case RetainedOutcome::CheckpointTrimmed:
            return 4;
    }
    throw std::invalid_argument("unknown RetainedOutcome in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(StreamDisposition value) {
    switch (value) {
        case StreamDisposition::Whole:
            return 0;
        case StreamDisposition::Rotated:
            return 1;
        case StreamDisposition::RemovalNotCarriedOut:
            return 2;
        case StreamDisposition::InterruptedRemoval:
            return 3;
        case StreamDisposition::GapAfterTrim:
            return 4;
        case StreamDisposition::PrefixMissing:
            return 5;
        case StreamDisposition::TrimNotOnSegmentBoundary:
            return 6;
        case StreamDisposition::Removed:
            return 7;
        case StreamDisposition::Absent:
            return 8;
    }
    throw std::invalid_argument("unknown StreamDisposition in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(BoundaryKind value) {
    switch (value) {
        case BoundaryKind::PredecessorMatched:
            return 0;
        case BoundaryKind::PredecessorNotReproduced:
            return 1;
        case BoundaryKind::PredecessorNotCheckable:
            return 2;
        case BoundaryKind::PredecessorFailedForm:
            return 3;
        case BoundaryKind::RecoveredMatched:
            return 4;
        case BoundaryKind::RecoveredNotReproduced:
            return 5;
        case BoundaryKind::RecoveredNotCheckable:
            return 6;
        case BoundaryKind::RecoveredFailedForm:
            return 7;
        case BoundaryKind::ClosedAt:
            return 8;
        case BoundaryKind::ClosedCitationNotReproduced:
            return 9;
        case BoundaryKind::EndedWithoutClose:
            return 10;
        case BoundaryKind::NoCloseRecorded:
            return 11;
        case BoundaryKind::LedgerNotClosed:
            return 12;
        case BoundaryKind::RemovedUnderRetention:
            return 13;
        case BoundaryKind::RemovedNeverAnchored:
            return 14;
        case BoundaryKind::RemovedWithoutAnchor:
            return 15;
        case BoundaryKind::LeftoverFromInterruptedRemoval:
            return 16;
        case BoundaryKind::RemovalNotCarriedOut:
            return 17;
        case BoundaryKind::PrefixMissingWithoutTrim:
            return 18;
        case BoundaryKind::RecordsMissingAfterTrim:
            return 19;
        case BoundaryKind::TrimPastAnchor:
            return 20;
        case BoundaryKind::OriginClaimedWhileHistoryExists:
            return 21;
        case BoundaryKind::NoEarlierHistoryKnown:
            return 22;
        case BoundaryKind::LedgersFork:
            return 23;
        case BoundaryKind::LedgerCitationCycle:
            return 24;
        case BoundaryKind::StreamNotOpenedByAnyLedger:
            return 25;
        case BoundaryKind::ReservedActionOutsideLedger:
            return 26;
        case BoundaryKind::LedgerRecordMalformed:
            return 27;
    }
    throw std::invalid_argument("unknown BoundaryKind in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(AuditResourceIssue value) {
    switch (value) {
        case AuditResourceIssue::None:
            return 0;
        case AuditResourceIssue::InvalidLimits:
            return 1;
        case AuditResourceIssue::Segments:
            return 2;
        case AuditResourceIssue::SegmentBytes:
            return 3;
        case AuditResourceIssue::TotalBytes:
            return 4;
        case AuditResourceIssue::ReadBytes:
            return 5;
        case AuditResourceIssue::Streams:
            return 6;
        case AuditResourceIssue::Records:
            return 7;
        case AuditResourceIssue::ProviderEntries:
            return 8;
        case AuditResourceIssue::ProviderCalls:
            return 9;
        case AuditResourceIssue::ProviderTextBytes:
            return 10;
        case AuditResourceIssue::IntegrityFaults:
            return 11;
        case AuditResourceIssue::MemoryUnavailable:
            return 12;
    }
    throw std::invalid_argument("unknown AuditResourceIssue in projection");
}
/** @brief Stable projection-v1 IDs, independent of C++ enumerator ordering. */
[[nodiscard]] constexpr std::uint64_t wireId(CanonicalReadStatus value) {
    switch (value) {
        case CanonicalReadStatus::Ok:
            return 0;
        case CanonicalReadStatus::Malformed:
            return 1;
        case CanonicalReadStatus::UnsupportedVersion:
            return 2;
    }
    throw std::invalid_argument("unknown CanonicalReadStatus in projection");
}

[[nodiscard]] constexpr std::uint64_t wireId(core::AuditCategory value) {
    switch (value) {
        case core::AuditCategory::Lifecycle:
            return 0;
        case core::AuditCategory::Configuration:
            return 1;
        case core::AuditCategory::Access:
            return 2;
        case core::AuditCategory::RiskControl:
            return 3;
        case core::AuditCategory::Operator:
            return 4;
    }
    throw std::invalid_argument("unknown event field in projection");
}
[[nodiscard]] constexpr std::uint64_t wireId(core::AuditPhase value) {
    switch (value) {
        case core::AuditPhase::Requested:
            return 0;
        case core::AuditPhase::Confirmed:
            return 1;
        case core::AuditPhase::Executed:
            return 2;
        case core::AuditPhase::Failed:
            return 3;
    }
    throw std::invalid_argument("unknown event field in projection");
}
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
inline constexpr unsigned char asciiEnd = 127;
/** @brief All text fields represent byte strings: non-ASCII bytes use U+00xx escapes, reversible by Latin-1 encoding. */
[[nodiscard]] inline std::string quote(std::string_view value) {
    std::string out = R"(")";
    for (const char byte : value) {
        const auto code = static_cast<unsigned char>(byte);
        if (code == '"' || code == '\\') {
            out += '\\';
            out += byte;
        } else if (code < ' ' || code >= asciiEnd)
            out += std::format("\\u{:04x}", code);
        else
            out += byte;
    }
    out += '"';
    return out;
}
[[nodiscard]] inline std::string hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (const auto byte : bytes)
        out += std::format("{:02x}", byte);
    return out;
}
template <typename T>
[[nodiscard]] std::string optionalNumber(const std::optional<T>& value) {
    return value ? std::to_string(*value) : "null";
}
[[nodiscard]] inline std::string time(core::RawTime value) {
    return value.availability() == core::TimeAvailability::Available ? std::to_string(value.value().time_since_epoch().count()) : "null";
}
[[nodiscard]] inline std::string anchor(const std::optional<Anchor>& value) {
    if (!value)
        return "null";
    return std::format(
        R"({{"anchorFormat":{},"canonicalVersion":{},"streamId":{},"position":{},"digest":{},"providerId":{},"counter":{},"acceptedTimeNs":{}}})",
        value->anchorFormat,
        value->canonicalVersion,
        quote(value->streamId),
        value->position,
        quote(hex(value->digest)),
        quote(value->providerId),
        value->counter,
        time(value->acceptedTime));
}
[[nodiscard]] inline std::string stream(const StreamReport& value) {
    return std::format(
        R"({{"streamId":{},"verdict":{},"verdictId":{},"causeId":{},"chainFindingId":{},"failedAt":{},"unknownLayoutVersion":{},"firstRetained":{)"
        R"(},"lastPresent":{},"anchoredThrough":{},"unanchoredFrom":{},"anchor":{},"ageId":{},"retainedId":{},"rollbackNotExcluded":{}}})",
        quote(value.streamId),
        quote(verdictName(value.verdict)),
        wireId(value.verdict),
        wireId(value.cause),
        wireId(value.chainFinding),
        optionalNumber(value.failedAt),
        optionalNumber(value.unknownLayoutVersion),
        value.firstRetained,
        value.lastPresent,
        optionalNumber(value.anchoredThrough),
        optionalNumber(value.unanchoredFrom),
        anchor(value.anchor),
        wireId(value.age),
        wireId(value.retained),
        value.rollbackNotExcluded());
}
}  // namespace projection

/** @brief A projection retains the whole-log report even when the event selection is partial. Never import it as evidence. */
[[nodiscard]] inline std::string auditProjectionJson(const AuditEvidence& evidence,
                                                     const LogReport&     report,
                                                     const LogImage&      image,
                                                     std::string_view     trust,
                                                     std::string_view     streamFilter  = {},
                                                     std::size_t          maxBytes      = defaultEvidenceBytes,
                                                     std::string_view     retainedTrust = "caller-supplied") {
    using namespace projection;
    const auto  exit = auditToolExit(report, image);
    std::string out  = std::format(
        R"({{"retainedTrust":{},"schema":"mddlog.audit.projection","version":{},"libraryVersion":{},"textEncoding":"byte-latin1","source":{},)"
         R"("providerProvenance":{},"trust":{},"selection":{{"complete":{},"streamId":{}}},"verificationTimeNs":{},"maxAnchorAgeNs":{},"exitCode":)"
         R"({},"report":{{"mediumUnreadable":{},"resourceIssueId":{},"unknownLayoutVersion":{},"segmentsWithoutHeader":{},"providerCalls":{},)"
         R"("usage":{{"segments":{},"totalBytes":{},"bytesRead":{},"readCalls":{},"records":{}}},"streams":[)",
        quote(retainedTrust),
        auditProjectionVersion,
        quote(MDDLOG_VERSION_STRING),
        quote(evidence.source),
        quote(evidence.providerProvenance),
        quote(trust),
        streamFilter.empty(),
        streamFilter.empty() ? "null" : quote(streamFilter),
        time(evidence.verification.verificationTime),
        evidence.verification.maxAnchorAge ? std::to_string(evidence.verification.maxAnchorAge->count()) : "null",
        std::to_underlying(exit),
        report.mediumUnreadable,
        wireId(report.resourceIssue),
        optionalNumber(image.unknownLayoutVersion()),
        image.segmentsWithoutHeader(),
        report.providerCalls,
        report.resourceUsage.segments,
        report.resourceUsage.totalBytes,
        report.resourceUsage.bytesRead,
        report.resourceUsage.readCalls,
        report.resourceUsage.records);
    const auto append = [&](std::string_view text) {
        if (out.size() > maxBytes || text.size() > maxBytes - out.size())
            throw std::length_error("projection exceeds output budget");
        out += text;
    };
    bool first = true;
    for (const auto& value : report.streams) {
        if (!std::exchange(first, false))
            append(",");
        append(std::format(
            R"({{"report":{},"ledger":{},"dispositionId":{},"trailing":{}}})",
            stream(value.report),
            value.ledger,
            wireId(value.disposition),
            value.trailing
                ? std::format(R"({{"segmentIndex":{},"offset":{},"length":{}}})", value.trailing->segmentIndex, value.trailing->offset, value.trailing->length)
                : "null"));
    }
    append(R"(],"unlisted":[)");
    first = true;
    for (const auto& value : report.unlisted) {
        if (!std::exchange(first, false))
            append(",");
        append(stream(value));
    }
    append(R"(],"boundaries":[)");
    first = true;
    for (const auto& value : report.notes) {
        if (!std::exchange(first, false))
            append(",");
        append(std::format(R"({{"kindId":{},"kind":{},"streamId":{},"other":{},"position":{},"secondPosition":{},"ledger":{},"ledgerPosition":{}}})",
                           wireId(value.kind),
                           quote(boundaryName(value.kind)),
                           quote(value.stream),
                           quote(value.other),
                           value.position,
                           value.secondPosition,
                           quote(value.ledger),
                           value.ledgerPosition));
    }
    append(R"(]},"events":[)");
    first = true;
    for (const auto& [id, value] : image.streams()) {
        if (!streamFilter.empty() && id != streamFilter)
            continue;
        for (const auto& stored : value.records) {
            if (!std::exchange(first, false))
                append(",");
            const auto  decoded = decodeCanonical(stored.bytes);
            const auto& event   = decoded.record;
            append(std::format(R"({{"streamId":{},"canonicalVersion":{},"decodeStatusId":{},"canonicalHex":{},"digest":{},"fields":)",
                               quote(id),
                               decoded.version,
                               wireId(decoded.status),
                               quote(hex(stored.bytes)),
                               quote(hex(stored.digest))));
            if (decoded.status != CanonicalReadStatus::Ok) {
                append("null}");
                continue;
            }
            append(std::format(R"({{"sequence":{},"categoryId":{},"phaseId":{},"timeNs":{},"action":{},"actor":{},"target":{},"requirementRef":{})"
                               R"(,"riskRef":{},"correlationId":{},"sourceSequence":{},"detail":{},"detailTruncated":{}}}}})",
                               event.sequence,
                               wireId(event.category),
                               wireId(event.phase),
                               time(event.time),
                               quote(event.action),
                               quote(event.actor),
                               quote(event.target),
                               quote(event.requirementRef),
                               quote(event.riskRef),
                               quote(event.correlationId),
                               optionalNumber(event.sourceSequence),
                               quote(event.detail),
                               event.detailTruncated));
        }
    }
    append("]}\n");
    return out;
}
}  // namespace mddlog::adapter
