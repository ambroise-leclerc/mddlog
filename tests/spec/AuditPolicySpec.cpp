/** @brief Public audit admission remains independent of diagnostic logging and sinks. */

import std;
import speclab;
import mddlog;
import mddlog.log;
import mddlog.core.auditevent;

#include "../framework/RecordingSink.hpp"

namespace {

using namespace mddlog;

[[nodiscard]] AuditInput auditRequest(std::string_view detail = "patient data accessed") noexcept {
    return {.category       = AuditCategory::Access,
            .phase          = AuditPhase::Executed,
            .time           = RawTime::unavailable(),
            .action         = "DATA_ACCESS",
            .actor          = "user123",
            .target         = "device789",
            .riskRef        = "HAZ-ACCESS-01",
            .correlationId  = "device7:boot9:ui1:41",
            .sourceSequence = 41,
            .detail         = detail};
}

class RecordingAuditSink : public AuditSink {
public:
    [[nodiscard]] bool accept(const core::AuditEvent& event) override {
        records.push_back(event);
        return true;
    }
    std::vector<core::AuditEvent> records;
};

const speclab::Register auditConfiguration{"Public logAudit reports absent and invalid audit configuration", "unit", [] {
                                               return speclab::Test("audit-public-configuration")
                                                   .Then("an unbound or malformed producer ring refuses without diagnostic fallback",
                                                         [] {
                                                             speclab::core::Checks checks;
                                                             SimpleLogger          logger{"audit-configuration", false};
                                                             auto                  diagnostic = std::make_shared<spec::RecordingSink>();
                                                             logger.addSink(diagnostic);
                                                             const auto absent = logger.logAudit(auditRequest());
                                                             checks.expect(!absent.wasAdmitted()
                                                                               && absent.refusal()->reason == AuditRefusalReason::Unconfigured,
                                                                           "missing ring is an observable refusal");
                                                             AuditRing<1> invalid{"bad stream"};
                                                             logger.setAuditRing(invalid);
                                                             const auto malformed = logger.logAudit(auditRequest());
                                                             checks.expect(!malformed.wasAdmitted()
                                                                               && malformed.refusal()->reason == AuditRefusalReason::InvalidStream,
                                                                           "malformed stream identity is an observable refusal");
                                                             logger.clearAuditRing();
                                                             checks.expect(diagnostic->size() == 0, "neither failure becomes a diagnostic audit record");
                                                             checks.raise();
                                                         })
                                                   .Execute();
                                           }};

const speclab::Register auditRouting{
    "Public logAudit admits to the dedicated ring despite diagnostic filters",
    "unit",
    [] {
        return speclab::Test("audit-public-routing")
            .Then("disabled logger and disabled audit sink have separate observable outcomes",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<2>          ring{"device7:boot9:audit1"};
                      SimpleLogger          logger{"audit-routing", false};
                      auto                  diagnostic = std::make_shared<spec::RecordingSink>();
                      logger.addSink(diagnostic);
                      logger.setEnabled(false);
                      logger.setMinLevel(LogLevel::Fatal);
                      logger.setAuditRing(ring);
                      logger.info("diagnostic filtered");
                      const auto admitted = logger.logAudit(auditRequest());
                      checks.expect(admitted.wasAdmitted() && admitted.sequence() == 1, "audit admission ignores diagnostic settings");
                      checks.expect(diagnostic->size() == 0, "diagnostic sink receives no audit record");
                      AuditSinkAdapter adapter;
                      auto             auditSink = std::make_shared<RecordingAuditSink>();
                      adapter.addRing(ring);
                      adapter.setSink(auditSink);
                      auditSink->setEnabled(false);
                      const auto disabled = adapter.drainOnce();
                      checks.expect(disabled.status == AuditDrainStatus::DisabledSink && adapter.healthSnapshot().pendingInRings == 1,
                                    "disabled audit sink is observable and retains the event");
                      auditSink->setEnabled(true);
                      checks.expect(adapter.drainOnce().handedOff == 1 && auditSink->records.size() == 1, "configured audit sink receives the event");
                      if (auditSink->records.size() == 1) {
                          const auto& event = auditSink->records.front();
                          checks.expect(event.action() == "DATA_ACCESS" && event.actor() == "user123" && event.target() == "device789",
                                        "legacy action, user and device values reach the audit record");
                          checks.expect(event.riskRef() == "HAZ-ACCESS-01" && event.detail() == "patient data accessed",
                                        "stable hazard reference and descriptive detail remain distinct");
                      }
                      logger.clearAuditRing();
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditFacade{"Log facade uses the same explicit audit admission contract", "unit", [] {
                                        return speclab::Test("audit-facade-admission")
                                            .Then("the facade returns unconfigured, admission, and invalid-identifier results",
                                                  [] {
                                                      speclab::core::Checks checks;
                                                      Log::shutdown();
                                                      const auto missing = Log::logAudit(auditRequest());
                                                      checks.expect(!missing.wasAdmitted() && missing.refusal()->reason == AuditRefusalReason::Unconfigured,
                                                                    "global logger has no implicit audit ring");
                                                      AuditRing<2> ring{"device7:boot9:facade"};
                                                      Log::setAuditRing(ring);
                                                      const auto admitted = Log::logAudit(auditRequest());
                                                      checks.expect(admitted.wasAdmitted() && admitted.sequence() == 1, "facade admits to its configured ring");
                                                      auto invalid       = auditRequest();
                                                      invalid.action     = "invalid action";
                                                      const auto refused = Log::logAudit(invalid);
                                                      checks.expect(!refused.wasAdmitted() && refused.refusal()->reason == AuditRefusalReason::InvalidIdentifier
                                                                        && refused.refusal()->field == AuditField::Action,
                                                                    "facade names a rejected field");
                                                      auto retained = Log::getLogger();
                                                      Log::shutdown();
                                                      const auto afterShutdown = retained->logAudit(auditRequest());
                                                      checks.expect(!afterShutdown.wasAdmitted()
                                                                        && afterShutdown.refusal()->reason == AuditRefusalReason::Unconfigured,
                                                                    "shutdown clears the binding on retained logger handles");
                                                      const auto afterRestart = Log::logAudit(auditRequest());
                                                      checks.expect(!afterRestart.wasAdmitted()
                                                                        && afterRestart.refusal()->reason == AuditRefusalReason::Unconfigured,
                                                                    "a reinitialized global logger needs a new audit binding");
                                                      Log::setAuditRing(ring);
                                                      const auto rebound = Log::logAudit(auditRequest());
                                                      checks.expect(rebound.wasAdmitted() && rebound.sequence() == 2,
                                                                    "rebinding resumes admission on the same ring without resetting its sequence");
                                                      Log::clearAuditRing();
                                                      Log::shutdown();
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};

const speclab::Register auditLegacyFieldBoundaries{
    "Former logAudit arguments are validated through the public admission API",
    "unit",
    [] {
        return speclab::Test("audit-public-field-boundaries")
            .Then("action, actor, target and risk reference retain exact bytes or name the refusal",
                  [] {
                      speclab::core::Checks checks;
                      struct FieldCase {
                          AuditField       field;
                          std::size_t      capacity;
                          std::string_view AuditInput::* member;
                      };
                      const std::array fields{
                          FieldCase{        .field = AuditField::Action,      .capacity = core::auditActionCapacity,         .member = &AuditInput::action},
                          FieldCase{         .field = AuditField::Actor,       .capacity = core::auditActorCapacity,          .member = &AuditInput::actor},
                          FieldCase{        .field = AuditField::Target,      .capacity = core::auditTargetCapacity,         .member = &AuditInput::target},
                          FieldCase{       .field = AuditField::RiskRef,   .capacity = core::auditReferenceCapacity,        .member = &AuditInput::riskRef},
                          FieldCase{.field = AuditField::RequirementRef,   .capacity = core::auditReferenceCapacity, .member = &AuditInput::requirementRef},
                          FieldCase{ .field = AuditField::CorrelationId, .capacity = core::auditCorrelationCapacity,  .member = &AuditInput::correlationId}
                      };
                      for (const auto& field : fields) {
                          AuditRing<2> ring{"device7:boot9:limits"};
                          SimpleLogger logger{"audit-limits", false};
                          logger.setAuditRing(ring);
                          auto        input = auditRequest();
                          std::string value(field.capacity, 'x');
                          input.*field.member = value;
                          checks.expect(logger.logAudit(input).wasAdmitted(), "exact capacity admitted");
                          value.push_back('x');
                          input.*field.member   = value;
                          const auto longResult = logger.logAudit(input);
                          checks.expect(!longResult.wasAdmitted() && longResult.refusal()->reason == AuditRefusalReason::InvalidIdentifier
                                            && longResult.refusal()->field == field.field,
                                        "overflow names field before ring saturation");
                          value.assign("bad value");
                          input.*field.member      = value;
                          const auto invalidResult = logger.logAudit(input);
                          checks.expect(!invalidResult.wasAdmitted() && invalidResult.refusal()->field == field.field, "free text cannot enter identifier");
                          logger.clearAuditRing();
                      }
                      AuditRing<2> ring{"device7:boot9:detail"};
                      SimpleLogger logger{"audit-detail", false};
                      logger.setAuditRing(ring);
                      std::string detail(core::auditDetailCapacity + 1, 'd');
                      const auto  result = logger.logAudit(auditRequest(detail));
                      checks.expect(result.wasAdmitted() && result.detailTruncated(), "only descriptive text is shortened and flagged");
                      logger.clearAuditRing();
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
