/**
 * @brief Compiled before/after examples of docs/migration/v0.2-to-1.0.md (#121).
 *
 * Each `// [migration:NAME]` ... `// [/migration:NAME]` region is quoted verbatim by the guide;
 * tests/documentation/TestPublicSurface.py fails when the two drift apart. The program is built
 * in-tree, from the installed package and from a source archive, and runs every example.
 */
import std;
import mddlog;

namespace {

// [migration:diagnostic-before]
// v0.2: identifiers repeated positionally at every call.
void primeBefore(mddlog::SimpleLogger& logger) {
    logger.logMedical(mddlog::LogLevel::Info, "Priming", "pump", "nurse-7", "session-2", "pump-1");
    logger.logMedical(mddlog::LogLevel::Info, "Primed", "pump", "nurse-7", "session-2", "pump-1");
}
// [/migration:diagnostic-before]

// [migration:diagnostic-after]
// Retained: the host prepares the context once; business code passes only the message.
void primeAfter(mddlog::DiagnosticBinding<mddlog::SimpleLogger>& logger) {
    logger.info("Priming");
    logger.info("Primed");
}
// [/migration:diagnostic-after]

// [migration:bounded-composition]
// Retained: budgets are chosen at the composition point; admission is observable.
[[nodiscard]] bool composeDiagnostics() {
    mddlog::SimpleLogger logger("pump", mddlog::DiagnosticConfig{.messageCapacity = 64, .maxRecordBytes = 256}, false);
    if (logger.tryLog(mddlog::LogLevel::Info, "Ready") != mddlog::DiagnosticStatus::Success)
        return false;
    return logger.flushChecked().status == mddlog::DiagnosticStatus::Success && logger.health().admitted == 1;
}
// [/migration:bounded-composition]

// [migration:audit-before]
// v0.2: every invariant field is restated in each AuditInput.
[[nodiscard]] bool doseBefore(mddlog::SimpleLogger& logger) {
    return logger
        .logAudit({.category       = mddlog::AuditCategory::Configuration,
                   .phase          = mddlog::AuditPhase::Requested,
                   .action         = "pump.rate.change",
                   .actor          = "nurse-7",
                   .target         = "pump-1",
                   .requirementRef = "REQ-12",
                   .correlationId  = "call-9"})
        .wasAdmitted();
}
// [/migration:audit-before]

// [migration:audit-after]
// Retained: description and context are validated once; phase and time stay explicit.
template <std::size_t Capacity>
[[nodiscard]] bool doseAfter(mddlog::AuditRing<Capacity>& ring) {
    const auto description = mddlog::AuditDescription::create(
        {.category = mddlog::AuditCategory::Configuration, .action = "pump.rate.change", .requirementRef = "REQ-12"});
    const auto context = mddlog::AuditContext::create({.actor = "nurse-7", .target = "pump-1", .correlationId = "call-9"});
    if (!description || !context)
        return false;  // A refused identifier is handled before any emission.
    mddlog::AuditBinding audit(ring, *description, *context);
    return audit.record(mddlog::AuditPhase::Requested, mddlog::RawTime::unavailable()).wasAdmitted();
}
// [/migration:audit-after]

// [migration:version-query]
// Retained: query the version through the module; the MDDLOG_VERSION_* macros are no longer
// defined in consumer translation units.
[[nodiscard]] bool versionKnown() {
    return !mddlog::getVersion().empty();
}
// [/migration:version-query]

}  // namespace

int main() {
    mddlog::SimpleLogger logger("migration", false);
    primeBefore(logger);
    const auto context = mddlog::DiagnosticContext::create({.component = "pump", .operationId = "prime", .correlationId = "call-9"});
    if (!context)
        return 1;
    mddlog::DiagnosticBinding binding(logger, *context);
    primeAfter(binding);
    if (logger.health().admitted != 4)
        return 2;
    if (!composeDiagnostics())
        return 3;

    mddlog::AuditRing<2> ring("migration:boot-1");
    logger.setAuditRing(ring);
    const bool before = doseBefore(logger);
    logger.clearAuditRing();
    if (!before || !doseAfter(ring))
        return 4;
    const auto events = ring.drain();
    if (events.size() != 2 || events.first()[0].action() != events.first()[1].action() || events.first()[0].actor() != events.first()[1].actor()
        || events.first()[0].requirementRef() != events.first()[1].requirementRef())
        return 5;
    if (!versionKnown())
        return 6;
    return 0;
}
