/** @brief Consume the public context API solely through the full installed/source umbrella. */
import std;
import mddlog;

int main() {
    const auto context      = mddlog::DiagnosticContext::create({.component = "consumer", .operationId = "operation", .correlationId = "call-1"});
    const auto description  = mddlog::AuditDescription::create({.action = "consumer.action"});
    const auto auditContext = mddlog::AuditContext::create({.actor = "host", .target = "consumer", .correlationId = "call-1"});
    if (!context || !description || !auditContext)
        return 1;
    mddlog::SimpleLogger logger("source-installed-context", mddlog::DiagnosticConfig{.messageCapacity = 4, .flushCapacity = 2, .maxRecordBytes = 128}, false);
    logger.setMinLevel(mddlog::LogLevel::Info);
    mddlog::DiagnosticBinding diagnostic(logger, *context);
    int                       constructions = 0;
    const auto                factory       = [&] {
        ++constructions;
        return std::string("temporary message");
    };
    diagnostic.debugLazy(factory);
    if (constructions != 0)
        return 2;
    diagnostic.logLazy(mddlog::LogLevel::Info, factory);
    if (constructions != 1)
        return 3;
    mddlog::AuditRing<1> ring("consumer:boot-1");
    mddlog::AuditBinding audit(ring, *description, *auditContext);
    if (!audit.record(mddlog::AuditPhase::Requested, mddlog::RawTime::unavailable()).wasAdmitted())
        return 4;
    const auto events = ring.drain();
    if (events.size() != 1 || events.first()[0].correlationId() != "call-1")
        return 5;
    const auto invalidContext = mddlog::DiagnosticContext::create({.component = std::string(256, 'x')});
    if (invalidContext)
        return 6;
    const mddlog::Refusal refusal = invalidContext.error();
    if (refusal.reason != mddlog::RefusalReason::IdentifierTooLong || refusal.field != mddlog::IdentifierField::Component)
        return 7;
    mddlog::RingLog<1>            diagnosticRing;
    mddlog::GovernedBinding       governed(diagnosticRing, *context);
    const mddlog::WriteResult     admitted  = governed.info(mddlog::RawTime::unavailable(), "governed message");
    const mddlog::TruncatedFields shortened = admitted.truncated();
    if (admitted.admission() != mddlog::Admission::Written || shortened.message)
        return 8;
    const mddlog::WriteResult full = governed.info(mddlog::RawTime::unavailable(), "full ring");
    if (full.admission() != mddlog::Admission::Refused || !full.refusal() || full.refusal()->reason != mddlog::RefusalReason::RingFull)
        return 9;
    if (logger.health().admitted != 1 || logger.health().processed != 1 || logger.configuration().messageCapacity != 4)
        return 10;
    const mddlog::DiagnosticFlushResult flush = logger.flushChecked();
    if (flush.status != mddlog::DiagnosticStatus::Success || !flush.sinks.empty())
        return 11;
    if (logger.shutdown().status != mddlog::DiagnosticStatus::Success || !logger.health().shutdownComplete)
        return 12;
    return 0;
}
