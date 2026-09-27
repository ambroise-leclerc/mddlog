/** @brief Runtime audit scenarios and boundary evidence for ADR-002. */

import std;
import mddlog.core.auditring;
import speclab;

namespace {

using namespace mddlog::core;

[[nodiscard]] AuditInput action(std::string_view correlation, std::uint64_t source, AuditPhase phase, RawTime time = RawTime::unavailable()) noexcept {
    return {.category       = AuditCategory::RiskControl,
            .phase          = phase,
            .time           = time,
            .action         = "TriggerHalt",
            .actor          = "operator_1",
            .target         = "emergency-halt",
            .requirementRef = "REQ-EM-003",
            .riskRef        = "HAZ-STOP-01",
            .correlationId  = correlation,
            .sourceSequence = source,
            .detail         = "critical control"};
}

const speclab::Register interleavedActions{
    "Interleaved critical actions retain independent correlations and audit order",
    "unit",
    [] {
        return speclab::Test("audit-interleaved-actions")
            .Then("each phase receives a fresh audit sequence while source sequence stays provenance",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<8>          ring{"device7:boot9:audit1"};
                      const std::array phases{AuditPhase::Requested, AuditPhase::Requested, AuditPhase::Confirmed, AuditPhase::Executed, AuditPhase::Failed};
                      const std::array<std::string_view, 5> correlations{"device7:boot9:ui1:41",
                                                                         "device7:boot9:ui1:42",
                                                                         "device7:boot9:ui1:41",
                                                                         "device7:boot9:ui1:41",
                                                                         "device7:boot9:ui1:42"};
                      const std::array<std::uint64_t, 5>    sources{41, 42, 41, 41, 42};
                      for (std::size_t i = 0; i < phases.size(); ++i) {
                          const auto result = ring.tryRecord(action(correlations.at(i), sources.at(i), phases.at(i)));
                          checks.expect(result.wasAdmitted() && result.sequence() == i + 1, "fresh audit sequence");
                      }
                      const auto view = ring.drain();
                      checks.expect(view.first().size() == phases.size(), "all interleaved events are present");
                      if (view.first().size() == phases.size()) {
                          for (std::size_t i = 0; i < phases.size(); ++i) {
                              const auto& event = view.first()[i];
                              checks.expect(event.streamId() == "device7:boot9:audit1" && event.sequence() == i + 1, "stream and sequence identify each event");
                              checks.expect(event.phase() == phases.at(i) && event.sourceSequence() == sources.at(i), "phase and provenance retained");
                              checks.expect(event.correlationId() == correlations.at(i), "source identity and sequence remain in correlation");
                              checks.expect(event.requirementRef() == "REQ-EM-003" && event.target() == "emergency-halt",
                                            "ActionTrace fields are copied without MduX dependency");
                          }
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register timeAndIdentity{
    "Audit order survives repeated and regressing civil time, concurrent streams and restart",
    "unit",
    [] {
        return speclab::Test("audit-time-and-identity")
            .Then(
                "time is data and producer instance identifies the sequence scope",
                [] {
                    speclab::core::Checks checks;
                    using namespace std::chrono;
                    const auto                      t       = RawTime::available(sys_time<nanoseconds>{seconds{100}});
                    const auto                      earlier = RawTime::available(sys_time<nanoseconds>{seconds{98}});
                    AuditRing<4>                    producerA{"device7:boot9:auditA1"};
                    AuditRing<2>                    producerB{"device7:boot9:auditB1"};
                    AuditRing<1>                    recreated{"device7:boot9:auditA2"};
                    AuditRing<1>                    restarted{"device7:boot10:auditA1"};
                    std::barrier                    start{3};
                    std::optional<AuditWriteResult> firstA;
                    std::optional<AuditWriteResult> firstB;
                    std::jthread                    a([&] {
                        start.arrive_and_wait();
                        firstA = producerA.tryRecord(action("device7:boot9:uiA:1", 1, AuditPhase::Requested, t));
                    });
                    std::jthread                    b([&] {
                        start.arrive_and_wait();
                        firstB = producerB.tryRecord(action("device7:boot9:uiB:1", 1, AuditPhase::Requested, t));
                    });
                    start.arrive_and_wait();
                    a.join();
                    b.join();
                    checks.expect(firstA.has_value() && firstA->wasAdmitted() && firstA->sequence() == 1, "first time admitted");
                    checks.expect(firstB.has_value() && firstB->wasAdmitted() && firstB->sequence() == 1, "concurrent producer has its own sequence");
                    checks.expect(producerA.tryRecord(action("device7:boot9:uiA:2", 2, AuditPhase::Requested, t)).sequence() == 2,
                                  "identical time does not merge events");
                    checks.expect(producerA.tryRecord(action("device7:boot9:uiA:3", 3, AuditPhase::Failed, earlier)).sequence() == 3,
                                  "clock regression does not change order");
                    checks.expect(producerA.tryRecord(action("device7:boot9:uiA:4", 4, AuditPhase::Requested)).sequence() == 4, "unavailable time is admitted");
                    checks.expect(recreated.tryRecord(action("device7:boot9:uiA2:1", 1, AuditPhase::Requested)).sequence() == 1,
                                  "recreated producer restarts under a new stream");
                    checks.expect(restarted.tryRecord(action("device7:boot10:uiA:1", 1, AuditPhase::Requested)).sequence() == 1,
                                  "new boot restarts under a new stream");
                    const auto events     = producerA.drain().first();
                    const auto other      = producerB.drain().first();
                    const auto recreation = recreated.drain().first();
                    const auto restart    = restarted.drain().first();
                    checks.expect(events.size() == 4 && other.size() == 1 && recreation.size() == 1 && restart.size() == 1, "every producer event is present");
                    if (events.size() == 4 && other.size() == 1 && recreation.size() == 1 && restart.size() == 1) {
                        checks.expect(events[0].time().value() == events[1].time().value(), "same timestamp retained");
                        checks.expect(events[2].time().value() < events[1].time().value() && events[2].sequence() > events[1].sequence(),
                                      "sequence orders backward clock correction");
                        checks.expect(events[3].time().availability() == TimeAvailability::Unavailable, "missing clock stays explicit");
                        checks.expect(events[0].streamId() != other[0].streamId() && events[0].streamId() != recreation[0].streamId()
                                          && events[0].streamId() != restart[0].streamId(),
                                      "equal sequence values have distinct stream identities");
                        checks.expect(events[0].correlationId() != other[0].correlationId(), "equal source sequence values do not collide across sources");
                    }
                    checks.raise();
                })
            .Execute();
    }};

const speclab::Register fieldBoundaries{
    "Every audit identifier is exact and only detail may shorten",
    "unit",
    [] {
        return speclab::Test("audit-field-boundaries")
            .Then("each field accepts its capacity and refuses an extra byte or invalid grammar",
                  [] {
                      speclab::core::Checks checks;
                      struct FieldCase {
                          AuditField       field;
                          std::size_t      capacity;
                          std::string_view AuditInput::* member;
                      };
                      const std::array fields{
                          FieldCase{        .field = AuditField::Action,      .capacity = auditActionCapacity,         .member = &AuditInput::action},
                          FieldCase{         .field = AuditField::Actor,       .capacity = auditActorCapacity,          .member = &AuditInput::actor},
                          FieldCase{        .field = AuditField::Target,      .capacity = auditTargetCapacity,         .member = &AuditInput::target},
                          FieldCase{.field = AuditField::RequirementRef,   .capacity = auditReferenceCapacity, .member = &AuditInput::requirementRef},
                          FieldCase{       .field = AuditField::RiskRef,   .capacity = auditReferenceCapacity,        .member = &AuditInput::riskRef},
                          FieldCase{ .field = AuditField::CorrelationId, .capacity = auditCorrelationCapacity,  .member = &AuditInput::correlationId}
                      };
                      for (const auto& field : fields) {
                          AuditRing<1> ring{"device7:boot9:audit1"};
                          std::string  value(field.capacity, 'a');
                          auto         input  = action("device7:boot9:ui1:41", 41, AuditPhase::Requested);
                          input.*field.member = value;
                          checks.expect(ring.tryRecord(input).wasAdmitted(), "identifier accepts exact capacity");
                          value.push_back('a');
                          input.*field.member   = value;
                          const auto longResult = ring.tryRecord(input);
                          checks.expect(!longResult.wasAdmitted() && longResult.refusal()->reason == AuditRefusalReason::InvalidIdentifier
                                            && longResult.refusal()->field == field.field,
                                        "identifier overflow names its field before saturation");
                          value.assign("bad value");
                          input.*field.member      = value;
                          const auto grammarResult = ring.tryRecord(input);
                          checks.expect(!grammarResult.wasAdmitted() && grammarResult.refusal()->field == field.field,
                                        "identifier grammar failure names its field");
                      }
                      const std::string validStream(auditStreamCapacity, 's');
                      const std::string longStream(auditStreamCapacity + 1, 's');
                      AuditRing<1>      valid{validStream};
                      AuditRing<1>      overlong{longStream};
                      AuditRing<1>      invalid{"bad stream"};
                      checks.expect(valid.tryRecord(action("src:1", 1, AuditPhase::Requested)).wasAdmitted(), "stream accepts exact capacity");
                      const auto overlongResult = overlong.tryRecord(action("src:1", 1, AuditPhase::Requested));
                      checks.expect(!overlongResult.wasAdmitted() && overlongResult.refusal()->reason == AuditRefusalReason::InvalidStream,
                                    "stream overflow is refused");
                      const auto invalidResult = invalid.tryRecord(action("src:1", 1, AuditPhase::Requested));
                      checks.expect(!invalidResult.wasAdmitted() && invalidResult.refusal()->reason == AuditRefusalReason::InvalidStream,
                                    "stream grammar is refused");
                      for (const auto field : {AuditField::Action, AuditField::Target}) {
                          auto emptyInput = action("src:1", 1, AuditPhase::Requested);
                          if (field == AuditField::Action)
                              emptyInput.action = "";
                          else
                              emptyInput.target = "";
                          AuditRing<1> required{"device7:boot9:required"};
                          const auto   requiredResult = required.tryRecord(emptyInput);
                          checks.expect(!requiredResult.wasAdmitted() && requiredResult.refusal()->field == field, "required identifier cannot be empty");
                      }
                      AuditRing<1> emptyStream{""};
                      const auto   emptyStreamResult = emptyStream.tryRecord(action("src:1", 1, AuditPhase::Requested));
                      checks.expect(!emptyStreamResult.wasAdmitted() && emptyStreamResult.refusal()->reason == AuditRefusalReason::InvalidStream,
                                    "stream identity cannot be empty");
                      const std::string maximalCorrelation = validStream + ":" + std::to_string(std::numeric_limits<std::uint64_t>::max());
                      AuditRing<1>      maximum{"device7:boot9:maximum"};
                      auto              maxInput = action(maximalCorrelation, std::numeric_limits<std::uint64_t>::max(), AuditPhase::Requested);
                      checks.expect(maximalCorrelation.size() == auditCorrelationCapacity && maximum.tryRecord(maxInput).wasAdmitted(),
                                    "maximum source stream and sequence fit correlation capacity");
                      AuditRing<2>      detailRing{"device7:boot9:detail"};
                      auto              input = action("src:1", 1, AuditPhase::Requested);
                      const std::string exact(auditDetailCapacity, 'd');
                      const std::string longDetail(auditDetailCapacity + 1, 'd');
                      input.detail           = exact;
                      const auto exactResult = detailRing.tryRecord(input);
                      checks.expect(exactResult.wasAdmitted() && !exactResult.detailTruncated(), "exact detail is admitted whole");
                      input.detail         = longDetail;
                      const auto shortened = detailRing.tryRecord(input);
                      const auto view      = detailRing.drain();
                      checks.expect(shortened.wasAdmitted() && shortened.detailTruncated(), "overlong detail is admitted and flagged");
                      checks.expect(view.size() == 2, "both detail records are present");
                      if (view.size() == 2)
                          checks.expect(view.first()[0].detail() == exact && view.first()[1].detail() == exact,
                                        "exact detail is preserved and overlong detail is shortened");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
