/** @brief Emit representative governed template bodies for the separate object scans. */
import std;
import mddlog.core.ring;
import mddlog.core.auditring;
import mddlog.core.governedbinding;
import mddlog.core.auditbinding;

template class mddlog::core::RingLog<1>;
template class mddlog::core::RingLog<3>;
template class mddlog::core::AuditRing<1>;
template class mddlog::core::AuditRing<3>;
template class mddlog::core::InlineString<mddlog::core::messageCapacity>;

// NOLINTNEXTLINE(misc-use-internal-linkage): external linkage keeps this scan probe emitted in optimized builds.
[[nodiscard]] mddlog::core::WriteResult assignGoverned(mddlog::core::GovernedRecord& record, const mddlog::core::RecordInput& input) noexcept {
    return record.assign(input);
}

template class mddlog::core::GovernedBinding<1>;
template class mddlog::core::AuditBinding<1>;

// NOLINTNEXTLINE(misc-use-internal-linkage): external linkage exercises nonconstant factory input in the object scans.
[[nodiscard]] std::expected<mddlog::core::DiagnosticContext, mddlog::core::Refusal>
captureDiagnosticContext(mddlog::core::DiagnosticContextOptions options) noexcept {
    return mddlog::core::DiagnosticContext::create(options);
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external linkage exercises nonconstant factory input in the object scans.
[[nodiscard]] std::expected<mddlog::core::AuditDescription, mddlog::core::AuditRefusal>
captureAuditDescription(mddlog::core::AuditDescriptionOptions options) noexcept {
    return mddlog::core::AuditDescription::create(options);
}

// NOLINTNEXTLINE(misc-use-internal-linkage): external linkage exercises nonconstant factory input in the object scans.
[[nodiscard]] std::expected<mddlog::core::AuditContext, mddlog::core::AuditRefusal> captureAuditContext(mddlog::core::AuditContextOptions options) noexcept {
    return mddlog::core::AuditContext::create(options);
}
