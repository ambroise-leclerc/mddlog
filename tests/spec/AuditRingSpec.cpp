/** @brief Bounded audit admission, stream ordering, and identifier refusals. */

import std;
import mddlog.core.auditring;
import speclab;

namespace {

using mddlog::core::AuditCategory;
using mddlog::core::AuditField;
using mddlog::core::AuditInput;
using mddlog::core::AuditPhase;
using mddlog::core::AuditRefusalReason;
using mddlog::core::AuditRing;
using mddlog::core::RawTime;

[[nodiscard]] AuditInput request(std::string_view detail = "requested") noexcept {
    return {.category       = AuditCategory::RiskControl,
            .phase          = AuditPhase::Requested,
            .time           = RawTime::unavailable(),
            .action         = "EMERGENCY_SHUTDOWN",
            .actor          = "operator_1",
            .target         = "device_789",
            .correlationId  = "device:boot:input:41",
            .sourceSequence = 41,
            .detail         = detail};
}

const speclab::Register auditAdmissionAndSequence{
    "AuditRing assigns sequences only to admitted events",
    "unit",
    [] {
        return speclab::Test("audit-admission-sequence")
            .Then("saturation refuses new events and acknowledgement frees the original slot",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<1>          ring{"device:boot:producer"};
                      const auto            first = ring.tryRecord(request());
                      const auto            full  = ring.tryRecord(request());
                      checks.expect(first.wasAdmitted() && first.sequence() == 1, "first event takes sequence 1");
                      checks.expect(!full.wasAdmitted() && full.refusal()->reason == AuditRefusalReason::RingFull, "second event is refused");
                      checks.expect(ring.refusalCount() == 1, "saturation is counted");
                      auto invalidInput   = request();
                      invalidInput.action = "has space";
                      checks.expect(ring.tryRecord(invalidInput).refusal()->field == AuditField::Action, "identifier validation precedes saturation");
                      const auto view = ring.drain();
                      checks.expect(view.size() == 1 && view.first()[0].sequence() == 1, "admitted event survives saturation");
                      checks.expect(view.first()[0].sourceSequence() == 41, "source sequence remains provenance");
                      checks.expect(view.first()[0].time().availability() == mddlog::core::TimeAvailability::Unavailable, "missing clock is represented");
                      checks.expect(ring.acknowledge(view, 1), "consumer releases the event");
                      const auto second = ring.tryRecord(request());
                      checks.expect(second.wasAdmitted() && second.sequence() == 2, "refusal did not consume sequence 2");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditFieldsAndIdentity{
    "AuditRing keeps identifiers exact and separates producer streams",
    "unit",
    [] {
        return speclab::Test("audit-fields-and-identity")
            .Then("invalid identifiers name their field and separate streams may start at one",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<2>          first{"device:boot:producerA"};
                      AuditRing<2>          second{"device:boot:producerB"};
                      auto                  input = request();
                      const std::string     tooLong(mddlog::core::auditActionCapacity + 1, 'x');
                      input.action       = tooLong;
                      const auto invalid = first.tryRecord(input);
                      checks.expect(!invalid.wasAdmitted() && invalid.refusal()->field == AuditField::Action, "action overflow is named");
                      input       = request();
                      input.actor = "has space";
                      checks.expect(first.tryRecord(input).refusal()->field == AuditField::Actor, "invalid actor grammar is named");
                      input        = request();
                      const auto a = first.tryRecord(input);
                      const auto b = second.tryRecord(input);
                      checks.expect(a.sequence() == 1 && b.sequence() == 1, "streams each start at one");
                      checks.expect(first.drain().first()[0].streamId() != second.drain().first()[0].streamId(), "stream identity separates events");
                      AuditRing<1> recreated{"device:boot:producerA2"};
                      checks.expect(recreated.tryRecord(input).sequence() == 1, "recreated producer starts at one under a new identity");
                      checks.expect(recreated.drain().first()[0].streamId() != first.drain().first()[0].streamId(), "recreation has a different stream");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditDetailAndPhases{"AuditRing retains phase and flags descriptive truncation", "unit", [] {
                                                 return speclab::Test("audit-detail-and-phases")
                                                     .Then("a confirmed outcome has a fresh sequence and long detail is flagged",
                                                           [] {
                                                               speclab::core::Checks checks;
                                                               AuditRing<3>          ring{"device:boot:producer"};
                                                               auto                  input = request();
                                                               checks.expect(ring.tryRecord(input).sequence() == 1, "request admitted");
                                                               input.phase = AuditPhase::Confirmed;
                                                               checks.expect(ring.tryRecord(input).sequence() == 2, "confirmation has new sequence");
                                                               input.phase = AuditPhase::Executed;
                                                               const std::string longDetail(mddlog::core::auditDetailCapacity + 1, 'd');
                                                               input.detail       = longDetail;
                                                               const auto outcome = ring.tryRecord(input);
                                                               checks.expect(outcome.sequence() == 3 && outcome.detailTruncated(), "long detail is flagged");
                                                               const auto view = ring.drain();
                                                               checks.expect(view.first()[2].phase() == AuditPhase::Executed
                                                                                 && view.first()[2].detail().size() == mddlog::core::auditDetailCapacity,
                                                                             "outcome and bounded detail are stored");
                                                               checks.expect(view.first()[0].correlationId() == view.first()[2].correlationId(),
                                                                             "phases retain correlation");
                                                               checks.raise();
                                                           })
                                                     .Execute();
                                             }};

}  // namespace
