/** @brief Local application integration: overflow, refusal equivalence and no implicit audit phases. */
import std;
import speclab;
import mddlog.core.governedbinding;
import mddlog.core.auditbinding;

#include "../../examples/InventoryWorker.hpp"

namespace {
using namespace inventory;

const speclab::Register stockLimits{
    "Context integration: concrete stock mutation rejects underflow and overflow",
    "integration",
    [] {
        return speclab::Test("context-integration-stock-limits")
            .Then("only valid host mutations are Executed; invalid mutations are Failed",
                  [] {
                      speclab::core::Checks checks;
                      const auto            context  = DiagnosticContext::create({.component = "stock"});
                      const auto            event    = AuditDescription::create({.action = "stock.adjust"});
                      const auto            identity = AuditContext::create({.target = "warehouse"});
                      RingLog<8>            log;
                      AuditRing<8>          audit("stock:boot-1");
                      InventoryWorker       worker(GovernedBinding(log, *context), AuditBinding(audit, *event, *identity));
                      const auto            first = worker.apply(std::numeric_limits<int>::max(), RawTime::unavailable(), RawTime::unavailable());
                      checks.expect(first.changed && worker.quantity() == std::numeric_limits<int>::max(), "maximum valid stock");
                      const auto overflow = worker.apply(1, RawTime::unavailable(), RawTime::unavailable());
                      checks.expect(!overflow.changed && worker.quantity() == std::numeric_limits<int>::max(), "overflow rejected without mutation");
                      const auto underflow = worker.apply(std::numeric_limits<int>::min(), RawTime::unavailable(), RawTime::unavailable());
                      checks.expect(!underflow.changed && worker.quantity() == std::numeric_limits<int>::max(), "underflow rejected without mutation");
                      const auto events = audit.drain();
                      checks.expect(events.size() == 6 && events.first()[1].phase() == AuditPhase::Executed && events.first()[3].phase() == AuditPhase::Failed
                                        && events.first()[5].phase() == AuditPhase::Failed,
                                    "host phases match observed validation");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register refusalEquivalence{
    "Context integration: before and after retain mutation on outcome refusal and block on request refusal",
    "integration",
    [] {
        return speclab::Test("context-integration-refusal-equivalence")
            .Then("low-level calls and injected component have the same refusal policy",
                  [] {
                      speclab::core::Checks checks;
                      const auto            context  = DiagnosticContext::create({.component = "stock"});
                      const auto            event    = AuditDescription::create({.action = "stock.adjust"});
                      const auto            identity = AuditContext::create({.target = "warehouse"});
                      RingLog<1>            beforeLog;
                      RingLog<1>            afterLog;
                      AuditRing<1>          beforeAudit("before:boot-2");
                      AuditRing<1>          afterAudit("after:boot-2");
                      InventoryWorker       worker(GovernedBinding(afterLog, *context), AuditBinding(afterAudit, *event, *identity));
                      int                   quantity = 0;
                      for (int i = 0; i < 2; ++i) {
                          const auto before =
                              adjustBefore(beforeLog, beforeAudit, *context, *event, *identity, quantity, 2, RawTime::unavailable(), RawTime::unavailable());
                          const auto after = worker.apply(2, RawTime::unavailable(), RawTime::unavailable());
                          checks.expect(before.changed == after.changed && quantity == worker.quantity(), "same mutation");
                          checks.expect(before.request.wasAdmitted() == after.request.wasAdmitted() && before.outcome.has_value() == after.outcome.has_value(),
                                        "same admission visibility");
                          if (before.outcome && after.outcome)
                              checks.expect(!before.outcome->wasAdmitted() && !after.outcome->wasAdmitted(), "both outcomes refused");
                      }
                      checks.expect(quantity == 2 && afterAudit.admittedCount() == 1, "refused request cannot repeat completed action");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register interruptedScope{"Context integration: exceptions and early returns invent no audit outcome", "integration", [] {
                                             return speclab::Test("context-integration-interrupted-scope")
                                                 .Then("only the host's explicit Requested phases are published",
                                                       [] {
                                                           speclab::core::Checks checks;
                                                           const auto            event    = AuditDescription::create({.action = "stock.adjust"});
                                                           const auto            identity = AuditContext::create({.target = "warehouse"});
                                                           AuditRing<4>          ring("stock:boot-3");
                                                           auto                  enter = [&] {
                                                               AuditBinding binding(ring, *event, *identity);
                                                               return binding.record(AuditPhase::Requested, RawTime::unavailable());
                                                           };
                                                           checks.expect(enter().wasAdmitted(), "early return admits only request");
                                                           bool interrupted = false;
                                                           try {
                                                               AuditBinding binding(ring, *event, *identity);
                                                               checks.expect(binding.record(AuditPhase::Requested, RawTime::unavailable()).wasAdmitted(),
                                                                             "explicit request before exception");
                                                               throw std::runtime_error("host action interrupted");
                                                           } catch (const std::runtime_error&) {
                                                               interrupted = true;  // Host decides recovery later, without an inferred phase.
                                                           }
                                                           checks.expect(interrupted, "host observes the interruption");
                                                           const auto events = ring.drain();
                                                           checks.expect(events.size() == 2 && ring.admittedCount() == 2, "destructors added no event");
                                                           for (const auto& value : events.first())
                                                               checks.expect(value.phase() == AuditPhase::Requested,
                                                                             "no implicit Executed, Failed or Confirmed");
                                                           checks.raise();
                                                       })
                                                 .Execute();
                                         }};
}  // namespace
