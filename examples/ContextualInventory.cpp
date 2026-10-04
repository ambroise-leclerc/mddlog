/** @brief Concrete local stock component, before/after equivalence and producer/consumer lifecycle. */
import std;
import mddlog.core.governedbinding;
import mddlog.core.auditbinding;

#include "InventoryWorker.hpp"

namespace {
using namespace inventory;

/** @brief Fail the application scenario on a violated integration contract. */
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

/** @brief Copy host diagnostic identifiers and reject invalid composition. */
DiagnosticContext diagnosticContext(std::string_view correlation) {
    auto value = DiagnosticContext::create({.component = "inventory", .operationId = "adjust", .correlationId = correlation});
    require(value.has_value(), "diagnostic composition");
    return *value;
}
/** @brief Declare the local stock action vocabulary once at composition. */
AuditDescription description() {
    auto value = AuditDescription::create({.category = AuditCategory::Operator, .action = "inventory.adjust"});
    require(value.has_value(), "audit description composition");
    return *value;
}
/** @brief Copy actor, target and correlation into an independently owned audit context. */
AuditContext auditContext(std::string_view correlation) {
    auto value = AuditContext::create({.actor = "operator-1", .target = "warehouse-1", .correlationId = correlation});
    require(value.has_value(), "audit context composition");
    return *value;
}

// Composition owns destinations; all business objects are destroyed before those destinations.
/** @brief Compare concrete mutations and emitted facts between direct admission and bound APIs. */
void equivalence() {
    RingLog<8>      beforeLog;
    AuditRing<8>    beforeAudit("inventory-before:boot-1");
    RingLog<8>      afterLog;
    AuditRing<8>    afterAudit("inventory-after:boot-1");
    const auto      context  = diagnosticContext("adjust-1");
    const auto      event    = description();
    const auto      identity = auditContext("adjust-1");
    InventoryWorker worker(GovernedBinding(afterLog, context), AuditBinding(afterAudit, event, identity));
    int             beforeStock = 0;
    const auto      requestedAt = RawTime::unavailable();
    const auto      observedAt  = RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{42}});
    for (const int delta : {3, -4, -1, std::numeric_limits<int>::max()}) {
        const auto before = adjustBefore(beforeLog, beforeAudit, context, event, identity, beforeStock, delta, requestedAt, observedAt);
        const auto after  = worker.apply(delta, requestedAt, observedAt);
        require(before.changed == after.changed && beforeStock == worker.quantity(), "equivalent stock mutation");
        require(before.request.wasAdmitted() && after.request.wasAdmitted() && before.outcome && after.outcome && before.outcome->wasAdmitted()
                    && after.outcome->wasAdmitted(),
                "explicit phase admission");
    }
    // Consumers copy records before acknowledgement; neither business method knows about drains.
    const auto beforeEvents = beforeAudit.drain();
    const auto afterEvents  = afterAudit.drain();
    require(beforeEvents.size() == 8 && afterEvents.size() == 8, "four explicit requests and outcomes");
    for (std::size_t i = 0; i < 8; ++i) {
        const auto& a = beforeEvents.first()[i];
        const auto& b = afterEvents.first()[i];
        require(a.category() == b.category() && a.phase() == b.phase() && a.action() == b.action() && a.actor() == b.actor() && a.target() == b.target()
                    && a.correlationId() == b.correlationId() && a.requirementRef() == b.requirementRef() && a.riskRef() == b.riskRef()
                    && a.time().availability() == b.time().availability() && a.time().value() == b.time().value() && a.sourceSequence() == b.sourceSequence()
                    && a.sequence() == b.sequence(),
                "equivalent audit semantics");
    }
    require(afterEvents.first()[3].phase() == AuditPhase::Failed && afterEvents.first()[1].sourceSequence() == 1, "failure and request link");
    const auto diagnostics         = afterLog.drain();
    const auto originalDiagnostics = beforeLog.drain();
    require(diagnostics.size() == 4 && originalDiagnostics.size() == 4, "one diagnostic per attempted mutation");
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& record = diagnostics.first()[i];
        require(record.message() == originalDiagnostics.first()[i].message() && record.component() == "inventory" && record.operationId() == "adjust"
                    && record.correlationId() == "adjust-1" && std::string_view(record.location().file_name()).ends_with("InventoryWorker.hpp")
                    && std::string_view(record.location().function_name()).find("apply") != std::string_view::npos,
                "business caller and owned context");
    }
    require(beforeAudit.acknowledge(beforeEvents, beforeEvents.size()) && afterAudit.acknowledge(afterEvents, afterEvents.size())
                && beforeLog.acknowledge(originalDiagnostics, originalDiagnostics.size()) && afterLog.acknowledge(diagnostics, diagnostics.size()),
            "consumer acknowledgement");
}

/** @brief Exercise request refusal, outcome refusal and diagnostic saturation as distinct host facts. */
void refusals() {
    const auto   context  = diagnosticContext("adjust-2");
    const auto   event    = description();
    const auto   identity = auditContext("adjust-2");
    RingLog<1>   log;
    AuditRing<1> audit("inventory:boot-2");
    {
        InventoryWorker worker(GovernedBinding(log, context), AuditBinding(audit, event, identity));
        const auto      result = worker.apply(5, RawTime::unavailable(), RawTime::unavailable());
        require(result.changed && worker.quantity() == 5 && result.request.wasAdmitted() && result.outcome && !result.outcome->wasAdmitted(),
                "outcome refusal preserves completed mutation");
        if (result.outcome) {
            const auto failure = result.outcome->refusal();
            require(failure && failure->reason == AuditRefusalReason::RingFull, "exact outcome refusal reason");
        }
        const auto blocked = worker.apply(7, RawTime::unavailable(), RawTime::unavailable());
        require(!blocked.changed && worker.quantity() == 5 && !blocked.request.wasAdmitted() && !blocked.outcome && !blocked.diagnostic,
                "request refusal blocks mutation");
    }
    require(audit.admittedCount() == 1, "destruction never fabricates outcome");
    AuditRing<4>    independentAudit("inventory:boot-3");
    InventoryWorker worker(GovernedBinding(log, context), AuditBinding(independentAudit, event, identity));
    const auto      changed = worker.apply(2, RawTime::unavailable(), RawTime::unavailable());
    require(changed.changed && changed.outcome && changed.outcome->wasAdmitted() && changed.diagnostic && changed.diagnostic->admission() == Admission::Refused,
            "diagnostic saturation cannot suppress audit or mutation");
    const auto rejected = worker.apply(-3, RawTime::unavailable(), RawTime::unavailable());
    require(!rejected.changed && worker.quantity() == 2 && rejected.outcome && rejected.outcome->wasAdmitted(), "invalid delta leaves stock intact");
}

/** @brief Exercise copied bindings with concurrent consumers, slot reuse and explicit shutdown. */
void concurrentLifecycle() {
    constexpr int iterations = 200;
    // One diagnostic and one audit ring per producer, owned before any copied binding or thread.
    RingLog<4>        firstLog;
    RingLog<4>        secondLog;
    AuditRing<4>      firstAudit("inventory-first:boot-4");
    AuditRing<4>      secondAudit("inventory-second:boot-4");
    InventoryWorker   first(GovernedBinding(firstLog, diagnosticContext(std::string("first"))),
                          AuditBinding(firstAudit, description(), auditContext(std::string("first"))));
    InventoryWorker   second(GovernedBinding(secondLog, diagnosticContext(std::string("second"))),
                           AuditBinding(secondAudit, description(), auditContext(std::string("second"))));
    std::atomic<int>  firstAcknowledged{0};
    std::atomic<int>  secondAcknowledged{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> healthy{true};
    auto              produce = [&](auto worker, std::atomic<int>& acknowledged, const std::stop_token& cancellation) {
        for (int i = 0; i < iterations && !stop.load() && !cancellation.stop_requested(); ++i) {
            const auto result = worker.apply(1, RawTime::unavailable(), RawTime::unavailable());
            if (!result.changed || !result.outcome || !result.outcome->wasAdmitted() || !result.diagnostic
                || result.diagnostic->admission() != Admission::Written)
                healthy.store(false);
            // Application scheduling waits for the consumer; the logging API itself never waits.
            while (acknowledged.load() <= i && !stop.load() && !cancellation.stop_requested())
                std::this_thread::yield();
        }
        if (!stop.load() && worker.quantity() != iterations)
            healthy.store(false);
    };
    std::jthread                firstProducer([&](const std::stop_token& cancellation) {
        produce(first, firstAcknowledged, cancellation);
    });
    std::jthread                secondProducer([&](const std::stop_token& cancellation) {
        produce(second, secondAcknowledged, cancellation);
    });
    std::vector<GovernedRecord> diagnostics;
    std::vector<AuditEvent>     events;
    auto                        consume = [&](auto& log, auto& audit, std::atomic<int>& acknowledged) {
        const auto diagnosticView = log.drain();
        const auto auditView      = audit.drain();
        if (diagnosticView.size() != 1 || auditView.size() != 2)
            return;
        for (const auto span : {diagnosticView.first(), diagnosticView.second()})
            diagnostics.insert(diagnostics.end(), span.begin(), span.end());
        for (const auto span : {auditView.first(), auditView.second()})
            events.insert(events.end(), span.begin(), span.end());
        if (!log.acknowledge(diagnosticView, diagnosticView.size()) || !audit.acknowledge(auditView, auditView.size()))
            healthy.store(false);
        acknowledged.fetch_add(1);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (firstAcknowledged.load() < iterations || secondAcknowledged.load() < iterations) {
        consume(firstLog, firstAudit, firstAcknowledged);
        consume(secondLog, secondAudit, secondAcknowledged);
        if (std::chrono::steady_clock::now() >= deadline) {
            stop.store(true);
            break;
        }
        std::this_thread::yield();
    }
    // Quiesce producers, finish consumers, discard bindings, then destroy rings. Copies survive ack.
    firstProducer.join();
    secondProducer.join();
    require(!stop.load() && healthy.load() && diagnostics.size() == 400 && events.size() == 800, "concurrent admission, wrap-around and shutdown");
    std::uint64_t firstSequence  = 0;
    std::uint64_t secondSequence = 0;
    for (const auto& value : events) {
        auto&                  sequence            = value.streamId() == "inventory-first:boot-4" ? firstSequence : secondSequence;
        const std::string_view expectedCorrelation = value.streamId() == "inventory-first:boot-4" ? "first" : "second";
        require(value.correlationId() == expectedCorrelation && value.sequence() == ++sequence, "stream isolation and copied sequences");
        require(value.phase() == (sequence % 2 == 1 ? AuditPhase::Requested : AuditPhase::Executed), "explicit alternating phases");
        if (sequence % 2 == 0)
            require(value.sourceSequence() == sequence - 1, "outcome refers to own request");
    }
    require(firstSequence == 400 && secondSequence == 400, "both streams drained without global order");
    for (const auto& record : diagnostics)
        require(record.component() == "inventory" && (record.correlationId() == "first" || record.correlationId() == "second")
                    && record.message() == "Stock adjusted",
                "owned diagnostic snapshots after slot reuse");
    require(firstLog.drain().empty() && secondLog.drain().empty() && firstAudit.drain().empty() && secondAudit.drain().empty(),
            "final consumer drain before destruction");
}
}  // namespace

/** @brief Run the selected local integration scenario and report failure through the exit status. */
int main(int argc, char** argv) {
    try {
        const std::string_view scenario = argc > 1 ? std::span(argv, static_cast<std::size_t>(argc))[1] : "all";
        require(scenario == "all" || scenario == "equivalence" || scenario == "refusals" || scenario == "lifecycle", "unknown scenario");
        if (scenario == "all" || scenario == "equivalence")
            equivalence();
        if (scenario == "all" || scenario == "refusals")
            refusals();
        if (scenario == "all" || scenario == "lifecycle")
            concurrentLifecycle();
        std::println("Contextual inventory: {} passed", scenario);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
