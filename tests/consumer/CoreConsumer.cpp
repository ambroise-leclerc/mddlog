import std;
import mddlog.core.record;
import mddlog.core.ring;
import mddlog.core.governedbinding;
import mddlog.core.auditbinding;

int main() {
    using namespace mddlog::core;

    RingLog<2> ring;
    const auto first    = ring.tryWrite({.level         = LogLevel::Info,
                                         .time          = RawTime::unavailable(),
                                         .message       = "governed diagnostic",
                                         .component     = "consumer",
                                         .operationId   = "operation-1",
                                         .correlationId = "correlation-1"});
    const auto hostTime = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{123}};
    const auto second   = ring.tryWrite({.level = LogLevel::Warn, .time = RawTime::available(hostTime), .message = "governed warning"});
    const auto full     = ring.tryWrite({.time = RawTime::unavailable(), .message = "full"});
    if (first.admission() != Admission::Written || second.admission() != Admission::Written || !full.refusal().has_value()
        || full.refusal()->reason != RefusalReason::RingFull) {
        return 1;
    }

    const auto view = ring.drain();
    if (view.size() != 2 || view.first().size() != 2 || view.first()[0].level() != LogLevel::Info || view.first()[0].message() != "governed diagnostic"
        || view.first()[0].component() != "consumer" || view.first()[0].operationId() != "operation-1" || view.first()[0].correlationId() != "correlation-1"
        || view.first()[0].time().availability() != TimeAvailability::Unavailable || view.first()[1].message() != "governed warning"
        || view.first()[1].time().availability() != TimeAvailability::Available || view.first()[1].time().value() != hostTime) {
        return 2;
    }
    if (!ring.acknowledge(view, view.size()) || !ring.drain().empty()) {
        return 3;
    }

    const auto afterReuse = ring.tryWrite({.time = RawTime::unavailable(), .message = "reused slot"});
    const auto nextView   = ring.drain();
    if (afterReuse.admission() != Admission::Written || nextView.size() != 1 || nextView.first()[0].message() != "reused slot"
        || !ring.acknowledge(nextView, 1)) {
        return 4;
    }
    if (!ring.drain().empty())
        return 5;
    const auto context      = DiagnosticContext::create({.component = "consumer", .operationId = "operation-2", .correlationId = "call-2"});
    const auto description  = AuditDescription::create({.category = AuditCategory::Operator, .action = "consumer.action"});
    const auto auditContext = AuditContext::create({.actor = "host", .target = "consumer", .correlationId = "call-2"});
    if (!context || !description || !auditContext)
        return 6;
    GovernedBinding diagnostic(ring, *context);
    if (diagnostic.info(RawTime::unavailable(), "bound diagnostic").admission() != Admission::Written)
        return 7;
    const auto diagnosticView = ring.drain();
    if (diagnosticView.size() != 1 || diagnosticView.first()[0].operationId() != "operation-2")
        return 8;
    AuditRing<2> auditRing("consumer:boot-1");
    AuditBinding audit(auditRing, *description, *auditContext);
    if (!audit.record(AuditPhase::Requested, RawTime::unavailable()).wasAdmitted()
        || !audit.record(AuditPhase::Executed, RawTime::available(hostTime), {.detail = "explicit outcome"}).wasAdmitted())
        return 9;
    const auto auditView = auditRing.drain();
    if (auditView.size() != 2 || auditView.first()[1].phase() != AuditPhase::Executed || auditView.first()[1].correlationId() != "call-2")
        return 10;
    std::cout << "context bytes: diagnostic=" << sizeof(DiagnosticContext) << ", governed=" << sizeof(GovernedBinding<2>)
              << ", description=" << sizeof(AuditDescription) << ", auditContext=" << sizeof(AuditContext) << ", auditBinding=" << sizeof(AuditBinding<2>)
              << '\n';
    return 0;
}
