/** @brief Executable before/after usage study using the delivered ADR-005 context bindings. */

import std;
import mddlog.core.ring;
import mddlog.core.auditring;
import mddlog.core.governedbinding;
import mddlog.core.auditbinding;
import mddlog.adapter.diagnosticbinding;

namespace study {
namespace {
using namespace mddlog::core;

using TextBinding = mddlog::adapter::DiagnosticBinding<mddlog::adapter::TextLogger>;

void require(bool condition, std::string_view failure) {
    if (!condition)
        throw std::runtime_error(std::string(failure));
}

// Before/after pairs keep configuration in main, outside the business functions.
void componentBefore(mddlog::adapter::TextLogger& logger) {
    logger.write(LogLevel::Info, "[pump::] Ready", std::source_location::current());
    logger.write(LogLevel::Info, "[pump::] Stopped", std::source_location::current());
}

void componentAfter(TextBinding& logger) {
    logger.info("Ready");
    logger.info("Stopped");
}

void operationBefore(mddlog::adapter::TextLogger& logger) {
    logger.write(LogLevel::Info, "[pump:prime:call-7] Started", std::source_location::current());
    logger.write(LogLevel::Info, "[pump:prime:call-7] Finished", std::source_location::current());
}

void operationAfter(TextBinding& logger) {
    logger.info("Started");
    logger.info("Finished");
}

[[nodiscard]] WriteResult governedBefore(RingLog<4>& ring, RawTime time) {
    return ring.tryWrite({.time = time, .location = std::source_location::current(), .message = "Ready", .component = "pump"});
}

[[nodiscard]] WriteResult governedAfter(GovernedBinding<4>& logger, RawTime time) {
    return logger.info(time, "Ready");
}

[[nodiscard]] AuditWriteResult auditWriteBefore(AuditRing<8>& ring, AuditPhase phase, RawTime time) {
    return ring.tryRecord({.category      = AuditCategory::Operator,
                           .phase         = phase,
                           .time          = time,
                           .action        = "pump.prime",
                           .actor         = "operator-1",
                           .target        = "pump-1",
                           .correlationId = "call-7"});
}

/** @brief Host policy result retains admission and the separately observed action outcome. */
struct [[nodiscard]] ActionAttempt {
    AuditWriteResult admission;
    bool             actionRan       = false;
    bool             actionSucceeded = false;
};

// A refused request blocks the action. A refused outcome cannot undo an action already performed.
template <typename Action>
[[nodiscard]] ActionAttempt auditBefore(AuditRing<8>& ring, RawTime time, Action&& action) {
    const auto request = auditWriteBefore(ring, AuditPhase::Requested, time);
    if (!request.wasAdmitted())
        return {.admission = request, .actionRan = false, .actionSucceeded = false};
    const bool succeeded = std::invoke(std::forward<Action>(action));
    const auto outcome   = auditWriteBefore(ring, succeeded ? AuditPhase::Executed : AuditPhase::Failed, time);
    return {.admission = outcome, .actionRan = true, .actionSucceeded = succeeded};
}

template <typename Action>
[[nodiscard]] ActionAttempt auditAfter(AuditBinding<8>& audit, RawTime time, Action&& action) {
    const auto request = audit.record(AuditPhase::Requested, time);
    if (!request.wasAdmitted())
        return {.admission = request, .actionRan = false, .actionSucceeded = false};
    const bool succeeded = std::invoke(std::forward<Action>(action));
    const auto outcome   = audit.record(succeeded ? AuditPhase::Executed : AuditPhase::Failed, time);
    return {.admission = outcome, .actionRan = true, .actionSucceeded = succeeded};
}

}  // namespace
}  // namespace study

// NOLINTNEXTLINE(bugprone-exception-escape): an uncaught study assertion fails the executable and CTest.
int main() {
    using namespace study;
    const auto time      = RawTime::unavailable();
    const auto component = DiagnosticContext::create({.component = "pump"});
    if (!component.has_value())
        throw std::runtime_error("valid component refused");
    const auto operation = component.value().withOperation({.operationId = "prime", .correlationId = std::string("call-7")});
    if (!operation.has_value())
        throw std::runtime_error("operation captures temporary identifiers");
    require(!DiagnosticContext::create({.component = std::string(componentCapacity + 1, 'x')}), "overlong component refused");

    mddlog::adapter::TextLogger text;
    text.set(LogLevel::Info, true);
    std::vector<std::string> lines;
    const auto               handle = text.addSink([&lines](std::string_view line) {
        lines.emplace_back(line);
    });
    TextBinding              componentLog(text, component.value());
    TextBinding              operationLog(text, operation.value());
    componentBefore(text);
    componentAfter(componentLog);
    operationBefore(text);
    operationAfter(operationLog);
    require(lines.size() == 8 && lines[2].ends_with("[pump::] Ready") && lines[6].ends_with("[pump:prime:call-7] Started"), "context projection");
    int        constructions    = 0;
    const auto expensiveMessage = [&constructions] {
        ++constructions;
        return std::string("expensive");
    };
    operationLog.debugLazy(expensiveMessage);
    require(constructions == 0, "disabled call skips message factory");
    text.set(LogLevel::Debug, true);
    operationLog.debugLazy(expensiveMessage);
    require(constructions == 1, "enabled call evaluates message factory once");
    text.removeSink(handle);

    RingLog<4>      beforeRing;
    RingLog<4>      afterRing;
    GovernedBinding afterLog(afterRing, component.value());
    require(governedBefore(beforeRing, time).admission() == Admission::Written, "before governed admission");
    require(governedAfter(afterLog, time).admission() == Admission::Written, "after governed admission");
    const auto expectedLine = std::source_location::current().line() + 1;
    require(afterLog.info(time, std::string("temporary")).admission() == Admission::Written, "temporary message admitted");
    const auto view = afterRing.drain();
    require(view.first()[1].location().line() == expectedLine && view.first()[1].message() == "temporary", "true caller and owned message");
    require(afterRing.acknowledge(view, view.size()), "consume outside business code");
    require(afterLog.info(time, std::string(messageCapacity + 1, 'x')).truncated().message, "truncation remains visible");

    // Multiple producers: the before API receives its own ring; the after API receives its binding.
    RingLog<4>      secondRing;
    GovernedBinding secondLog(secondRing, operation.value());
    require(governedAfter(secondLog, time).admission() == Admission::Written, "independent producer");
    require(secondRing.drain().first()[0].correlationId() == "call-7" && afterRing.drain().first()[0].correlationId().empty(), "independent contexts");

    AuditRing<8> beforeAudit("before:boot-1");
    require(auditBefore(beforeAudit,
                        time,
                        [] {
                            return true;
                        })
                .admission.wasAdmitted(),
            "before audit action");
    // The host owns its vocabulary. Description and operation identities are separate named options.
    const auto primeDescription = AuditDescription::create({.category = AuditCategory::Operator, .action = "pump.prime"});
    const auto auditContext     = AuditContext::create({.actor = "operator-1", .target = "pump-1", .correlationId = "call-7"});
    if (!primeDescription || !auditContext)
        throw std::runtime_error("invalid audit composition");
    AuditRing<8> auditRing("pump:boot-1");
    {
        AuditBinding audit(auditRing, *primeDescription, *auditContext);
        require(auditRing.admittedCount() == 0, "construction never publishes");
        require(auditAfter(audit,
                           time,
                           [] {
                               return true;
                           })
                    .admission.wasAdmitted(),
                "explicit success");
        require(auditAfter(audit,
                           time,
                           [] {
                               return false;
                           })
                    .admission.wasAdmitted(),
                "explicit failure");
        require(audit.record(AuditPhase::Confirmed, time).wasAdmitted(), "confirmation is explicit");
    }
    require(auditRing.admittedCount() == 5, "destruction never publishes");
    const auto events = auditRing.drain();
    require(events.first()[0].phase() == AuditPhase::Requested && events.first()[1].phase() == AuditPhase::Executed
                && events.first()[3].phase() == AuditPhase::Failed && events.first()[4].phase() == AuditPhase::Confirmed,
            "explicit phase sequence");
    AuditBinding audit(auditRing, *primeDescription, *auditContext);
    for (int i = 0; i < 2; ++i)
        require(audit.record(AuditPhase::Requested, time).wasAdmitted(), "fill audit ring");
    int        actions = 0;
    const auto action  = [&actions] {
        ++actions;
        return true;
    };
    const auto outcomeRefused = auditAfter(audit, time, action);
    require(!outcomeRefused.admission.wasAdmitted() && outcomeRefused.actionRan && outcomeRefused.actionSucceeded && actions == 1,
            "refused outcome cannot undo an executed action");
    const auto requestRefused = auditAfter(audit, time, action);
    const auto requestFailure = requestRefused.admission.refusal();
    require(requestFailure && requestFailure->reason == AuditRefusalReason::RingFull && !requestRefused.actionRan && actions == 1,
            "refused request blocks the action and preserves refusal reason");
    const auto invalid = AuditDescription::create({.action = "mddlog.reserved"});
    require(!invalid && invalid.error().reason == AuditRefusalReason::ReservedAction, "reserved vocabulary refused");
    std::println("ADR-005: five usage comparisons and prototype checks passed");
}
