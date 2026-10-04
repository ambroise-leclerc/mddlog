/** @brief Reusable host vocabulary, exact validation, owned identities and explicit audit outcomes. */
import std;
import speclab;
import mddlog.core.auditbinding;

namespace {
using namespace mddlog::core;
static_assert(!std::is_default_constructible_v<AuditDescription> && !std::is_default_constructible_v<AuditContext>);
static_assert(!std::is_constructible_v<AuditBinding<2>, AuditRing<2>&&, AuditDescription, AuditContext>);
static_assert(std::is_trivially_destructible_v<AuditBinding<2>>);

const speclab::Register validation{
    "Context: audit descriptions and identities validate every exact identifier",
    "unit",
    [] {
        return speclab::Test("context-audit-identifier-validation")
            .Then("host vocabulary is validated without a fabricated event",
                  [] {
                      speclab::core::Checks checks;
                      const auto            empty    = AuditDescription::create({});
                      const auto            reserved = AuditDescription::create({.action = "mddlog.host"});
                      checks.expect(!empty && empty.error().field == AuditField::Action, "action mandatory");
                      checks.expect(!reserved && reserved.error().reason == AuditRefusalReason::ReservedAction, "ledger namespace reserved");
                      for (const auto& [field, capacity] : std::array{
                               std::pair{        AuditField::Action,      auditActionCapacity},
                               std::pair{         AuditField::Actor,       auditActorCapacity},
                               std::pair{        AuditField::Target,      auditTargetCapacity},
                               std::pair{AuditField::RequirementRef,   auditReferenceCapacity},
                               std::pair{       AuditField::RiskRef,   auditReferenceCapacity},
                               std::pair{ AuditField::CorrelationId, auditCorrelationCapacity}
                      }) {
                          checks.expect(!AuditEvent::validateIdentifier(field, std::string(capacity, 'x')), "exact identifier accepted");
                          const auto failure = AuditEvent::validateIdentifier(field, std::string(capacity + 1, 'x'));
                          checks.expect(failure && failure->field == field && failure->reason == AuditRefusalReason::InvalidIdentifier,
                                        "overlong identifier names its field");
                          checks.expect(AuditEvent::validateIdentifier(field, "has a space").has_value(), "invalid bytes rejected");
                      }
                      const auto badRequirement = AuditDescription::create({.action = "host.action", .requirementRef = "bad ref"});
                      const auto badRisk        = AuditDescription::create({.action = "host.action", .riskRef = "bad ref"});
                      const auto badActor       = AuditContext::create({.actor = "bad actor", .target = "pump"});
                      const auto badTarget      = AuditContext::create({});
                      const auto badCorrelation = AuditContext::create({.target = "pump", .correlationId = "bad correlation"});
                      checks.expect(!badRequirement && badRequirement.error().field == AuditField::RequirementRef, "description requirement validation");
                      checks.expect(!badRisk && badRisk.error().field == AuditField::RiskRef, "description risk validation");
                      checks.expect(!badActor && badActor.error().field == AuditField::Actor, "context actor validation");
                      checks.expect(!badTarget && badTarget.error().field == AuditField::Target, "context target mandatory");
                      checks.expect(!badCorrelation && badCorrelation.error().field == AuditField::CorrelationId, "context correlation validation");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register capture{
    "Context: audit binding owns temporaries and every phase remains explicit",
    "unit",
    [] {
        return speclab::Test("context-audit-owned-phases-and-time")
            .Then("no constructor or destructor invents an execution",
                  [] {
                      speclab::core::Checks checks;
                      const auto            description = AuditDescription::create({.category       = AuditCategory::Operator,
                                                                                    .action         = std::string("pump.prime"),
                                                                                    .requirementRef = std::string("host:R1"),
                                                                                    .riskRef        = "host:K1"});
                      const auto            context     = AuditContext::create(
                          {.actor = std::string("operator-1"), .target = std::string("pump-1"), .correlationId = std::string("call-1")});
                      checks.expect(description.has_value() && context.has_value(), "owned invariants created");
                      if (description && context) {
                          AuditRing<4> ring("pump:boot-1");
                          {
                              AuditBinding binding(ring, *description, *context);
                              checks.expect(ring.admittedCount() == 0, "construction never publishes");
                              const auto time    = RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{321}});
                              const auto request = binding.record(AuditPhase::Requested, time, {.detail = std::string("temporary"), .sourceSequence = 7});
                              const auto confirm = binding.record(AuditPhase::Confirmed, time);
                              const auto execute = binding.record(AuditPhase::Executed, RawTime::unavailable());
                              const auto fail    = binding.record(AuditPhase::Failed, time, {.detail = std::string(auditDetailCapacity - 1, 'x') + "\xC3\xA9"});
                              checks.expect(request.wasAdmitted() && request.sequence() == 1 && confirm.wasAdmitted() && execute.wasAdmitted()
                                                && fail.wasAdmitted(),
                                            "each phase supplied explicitly");
                              checks.expect(fail.detailTruncated(), "UTF-8 detail truncation remains visible");
                          }
                          checks.expect(ring.admittedCount() == 4, "destruction never publishes");
                          const auto events = ring.drain();
                          if (events.size() == 4) {
                              const auto& event = events.first()[0];
                              checks.expect(event.action() == "pump.prime" && event.actor() == "operator-1" && event.target() == "pump-1"
                                                && event.correlationId() == "call-1",
                                            "owned identities");
                              checks.expect(event.requirementRef() == "host:R1" && event.riskRef() == "host:K1" && event.detail() == "temporary"
                                                && event.sourceSequence() == 7,
                                            "references and event data preserved");
                              checks.expect(event.streamId() == "pump:boot-1" && event.phase() == AuditPhase::Requested
                                                && events.first()[1].phase() == AuditPhase::Confirmed && events.first()[2].phase() == AuditPhase::Executed
                                                && events.first()[3].phase() == AuditPhase::Failed,
                                            "ring identity and explicit phases");
                              checks.expect(events.first()[2].time().availability() == TimeAvailability::Unavailable
                                                && events.first()[3].detail().size() == auditDetailCapacity - 1,
                                            "event time and whole UTF-8");
                          } else
                              checks.expect(false, "four events expected");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register refusalAndIndependence{
    "Context: audit copies preserve independent streams and explicit admission refusal",
    "unit",
    [] {
        return speclab::Test("context-audit-streams-and-refusal")
            .Then("each ring owns its sequence and borrowed invalid streams remain refusals",
                  [] {
                      speclab::core::Checks checks;
                      const auto            description = AuditDescription::create({.action = "host.action"});
                      const auto            parent      = AuditContext::create({.actor = "operator-1", .target = "pump-1", .correlationId = "parent"});
                      checks.expect(description.has_value() && parent.has_value(), "valid invariants");
                      if (description && parent) {
                          const auto child = AuditContext::create({.actor = parent->actor(), .target = "pump-2", .correlationId = "child"});
                          if (child) {
                              AuditRing<1> first("first:boot-1");
                              AuditRing<1> second("second:boot-1");
                              AuditRing<1> invalid("");
                              AuditBinding firstBinding(first, *description, *parent);
                              AuditBinding secondBinding(second, *description, *child);
                              AuditBinding invalidBinding(invalid, *description, *parent);
                              const auto   firstResult   = firstBinding.record(AuditPhase::Requested, RawTime::unavailable());
                              const auto   secondResult  = secondBinding.record(AuditPhase::Failed, RawTime::unavailable());
                              const auto   full          = firstBinding.record(AuditPhase::Executed, RawTime::unavailable());
                              const auto   broken        = invalidBinding.record(AuditPhase::Requested, RawTime::unavailable());
                              const auto   fullFailure   = full.refusal();
                              const auto   brokenFailure = broken.refusal();
                              checks.expect(firstResult.sequence() == 1 && secondResult.sequence() == 1, "sequences belong to each ring");
                              checks.expect(fullFailure && fullFailure->reason == AuditRefusalReason::RingFull && first.admittedCount() == 1,
                                            "refusal consumes no sequence");
                              checks.expect(brokenFailure && brokenFailure->reason == AuditRefusalReason::InvalidStream, "invalid stream reported at emission");
                              checks.expect(first.drain().first()[0].correlationId() == "parent" && second.drain().first()[0].correlationId() == "child"
                                                && parent->target() == "pump-1",
                                            "derived context independent");
                          } else
                              checks.expect(false, "child creation failed");
                      }
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
