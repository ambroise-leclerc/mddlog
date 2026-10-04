/** @brief Consume the public context API solely through the full installed/source umbrella. */
import std;
import mddlog;

int main() {
    const auto context      = mddlog::DiagnosticContext::create({.component = "consumer", .operationId = "operation", .correlationId = "call-1"});
    const auto description  = mddlog::AuditDescription::create({.action = "consumer.action"});
    const auto auditContext = mddlog::AuditContext::create({.actor = "host", .target = "consumer", .correlationId = "call-1"});
    if (!context || !description || !auditContext)
        return 1;
    mddlog::SimpleLogger logger("source-installed-context", false);
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
    return 0;
}
