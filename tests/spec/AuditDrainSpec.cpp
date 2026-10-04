/** @brief Audit-only hand-off, refusal retention, and asynchronous health observations. */

import std;
import speclab;
import mddlog;
import mddlog.core.auditring;

namespace {

using mddlog::AuditDrainStatus;
using mddlog::AuditRingRegistration;
using mddlog::AuditSink;
using mddlog::AuditSinkAdapter;
using mddlog::core::AuditCategory;
using mddlog::core::AuditInput;
using mddlog::core::AuditPhase;
using mddlog::core::AuditRing;

static_assert(!std::is_convertible_v<std::shared_ptr<mddlog::Sink>, mddlog::AuditSinkPtr>);

[[nodiscard]] AuditInput auditRequest() noexcept {
    return {.category       = AuditCategory::RiskControl,
            .phase          = AuditPhase::Requested,
            .action         = "INTERLOCK_OVERRIDE",
            .actor          = "operator_1",
            .target         = "interlock_1",
            .correlationId  = "device:boot:input:41",
            .sourceSequence = 41,
            .detail         = "override requested"};
}

class RecordingAuditSink : public AuditSink {
public:
    enum class Mode : std::uint8_t { Accept, Reject, Throw };

    [[nodiscard]] bool accept(const mddlog::core::AuditEvent& event) override {
        ++attempts;
        if (mode == Mode::Throw)
            throw std::runtime_error("audit sink unavailable");
        if (mode == Mode::Reject || event.sequence() == rejectSequence || event.streamId() == rejectStream)
            return false;
        records.push_back(event);
        return true;
    }

    Mode                                  mode           = Mode::Accept;
    std::uint64_t                         rejectSequence = 0;
    std::string_view                      rejectStream;
    std::size_t                           attempts = 0;
    std::vector<mddlog::core::AuditEvent> records;
};

const speclab::Register auditConfigurationIsObservable{
    "The audit consumer reports absent and disabled audit sinks without releasing events",
    "unit",
    [] {
        return speclab::Test("audit-configuration-errors")
            .Then("an audit event remains pending until a valid sink accepts it",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<1>          ring{"device:boot:producer"};
                      AuditSinkAdapter      adapter;
                      checks.expect(adapter.addRing(ring) == AuditRingRegistration::Registered, "ring registered");
                      checks.expect(ring.tryRecord(auditRequest()).wasAdmitted(), "event admitted before sink setup");
                      checks.expect(adapter.drainOnce().status == AuditDrainStatus::MissingSink, "missing sink is reported");
                      auto sink = std::make_shared<RecordingAuditSink>();
                      sink->setEnabled(false);
                      adapter.setSink(sink);
                      checks.expect(adapter.drainOnce().status == AuditDrainStatus::DisabledSink, "disabled sink is reported");
                      const auto before = adapter.healthSnapshot();
                      checks.expect(before.configurationErrors == 2 && before.pendingInRings == 1, "both errors and pending event are visible");
                      checks.expect(before.handedOff == 0 && sink->attempts == 0, "neither configuration error invokes a sink");
                      sink->setEnabled(true);
                      const auto delivered = adapter.drainOnce();
                      checks.expect(delivered.status == AuditDrainStatus::Completed && delivered.handedOff == 1, "reconfigured sink accepts pending event");
                      checks.expect(adapter.healthSnapshot().pendingInRings == 0, "acknowledgement releases the slot");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditIgnoresDisabledLogger{"The audit consumer ignores diagnostic logger disablement", "unit", [] {
                                                       return speclab::Test("audit-independent-of-disabled-logger")
                                                           .Then("disabling diagnostics does not filter the separate audit lane",
                                                                 [] {
                                                                     speclab::core::Checks checks;
                                                                     mddlog::SimpleLogger  diagnostic{"diagnostic", false};
                                                                     diagnostic.setEnabled(false);
                                                                     AuditRing<1>     ring{"device:boot:producer"};
                                                                     AuditSinkAdapter adapter;
                                                                     auto             sink = std::make_shared<RecordingAuditSink>();
                                                                     checks.expect(adapter.addRing(ring) == AuditRingRegistration::Registered,
                                                                                   "ring registered");
                                                                     adapter.setSink(sink);
                                                                     checks.expect(ring.tryRecord(auditRequest()).wasAdmitted(),
                                                                                   "audit admitted with diagnostics disabled");
                                                                     checks.expect(adapter.drainOnce().handedOff == 1 && sink->records.size() == 1,
                                                                                   "audit still reaches its own sink");
                                                                     checks.expect(!diagnostic.isEnabled(), "diagnostic logger remains disabled");
                                                                     checks.raise();
                                                                 })
                                                           .Execute();
                                                   }};

const speclab::Register auditIgnoresDiagnosticThreshold{
    "The audit consumer ignores the diagnostic severity threshold",
    "unit",
    [] {
        return speclab::Test("audit-independent-of-diagnostic-threshold")
            .Then("a restrictive diagnostic threshold does not filter the separate audit lane",
                  [] {
                      speclab::core::Checks checks;
                      mddlog::SimpleLogger  diagnostic{"diagnostic", false};
                      diagnostic.setMinLevel(mddlog::LogLevel::Fatal);
                      AuditRing<1>     ring{"device:boot:producer"};
                      AuditSinkAdapter adapter;
                      auto             sink = std::make_shared<RecordingAuditSink>();
                      checks.expect(adapter.addRing(ring) == AuditRingRegistration::Registered, "ring registered");
                      adapter.setSink(sink);
                      checks.expect(ring.tryRecord(auditRequest()).wasAdmitted(), "audit admitted with restrictive diagnostic threshold");
                      checks.expect(adapter.drainOnce().handedOff == 1 && sink->records.size() == 1, "audit still reaches its own sink");
                      checks.expect(diagnostic.isEnabled() && diagnostic.getMinLevel() == mddlog::LogLevel::Fatal,
                                    "only the diagnostic threshold is restrictive");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditFailureRetainsAdmission{
    "Audit sink rejection and exception retain admitted records and expose independent health",
    "unit",
    [] {
        return speclab::Test("audit-failure-and-retry")
            .Then("saturation followed by sink failure never overwrites the admitted event",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<1>          ring{"device:boot:producer"};
                      AuditSinkAdapter      adapter;
                      auto                  sink = std::make_shared<RecordingAuditSink>();
                      checks.expect(adapter.addRing(ring) == AuditRingRegistration::Registered, "ring registered");
                      adapter.setSink(sink);
                      checks.expect(ring.tryRecord(auditRequest()).sequence() == 1, "first event admitted");
                      checks.expect(!ring.tryRecord(auditRequest()).wasAdmitted() && ring.refusalCount() == 1, "saturation is refused immediately");
                      sink->mode = RecordingAuditSink::Mode::Reject;
                      checks.expect(adapter.drainOnce().status == AuditDrainStatus::SinkRejected, "rejection is observable");
                      auto afterReject = adapter.healthSnapshot();
                      checks.expect(afterReject.dispatchFailures == 1 && afterReject.takenUnacknowledged == 1 && afterReject.pendingInRings == 1,
                                    "rejected event is still pending with a failure signal");
                      sink->mode = RecordingAuditSink::Mode::Throw;
                      checks.expect(adapter.drainOnce().status == AuditDrainStatus::SinkThrew, "exception is observable");
                      checks.expect(adapter.healthSnapshot().dispatchFailures == 2 && adapter.healthSnapshot().takenUnacknowledged == 1,
                                    "repeat failure increments attempts but not distinct unacknowledged events");
                      checks.expect(ring.drain().first()[0].sequence() == 1, "admitted event was not overwritten");
                      sink->mode = RecordingAuditSink::Mode::Accept;
                      checks.expect(adapter.drainOnce().handedOff == 1, "retry hands off the original event");
                      auto recovered = adapter.healthSnapshot();
                      checks.expect(recovered.takenUnacknowledged == 0 && recovered.pendingInRings == 0 && recovered.handedOff == 1,
                                    "successful acknowledgement clears pending failure");
                      adapter.reportLoss();
                      checks.expect(adapter.healthSnapshot().reportedLosses == 1, "later sink-side loss has its own signal");
                      checks.expect(ring.tryRecord(auditRequest()).sequence() == 2, "acknowledgement makes capacity available");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditPartialAcknowledgement{
    "Audit consumer releases only the accepted prefix and resumes with the rejected event",
    "unit",
    [] {
        return speclab::Test("audit-partial-acknowledgement")
            .Then("a rejection after one hand-off leaves the rejected event and following records queued",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<3>          ring{"device:boot:producer"};
                      AuditSinkAdapter      adapter;
                      auto                  sink = std::make_shared<RecordingAuditSink>();
                      sink->rejectSequence       = 2;
                      checks.expect(adapter.addRing(ring) == AuditRingRegistration::Registered, "ring registered");
                      adapter.setSink(sink);
                      for (int index = 0; index < 3; ++index)
                          checks.expect(ring.tryRecord(auditRequest()).wasAdmitted(), "event admitted");
                      const auto first = adapter.drainOnce();
                      checks.expect(first.handedOff == 1 && first.status == AuditDrainStatus::SinkRejected, "only first event was acknowledged");
                      checks.expect(ring.drain().first()[0].sequence() == 2 && adapter.healthSnapshot().pendingInRings == 2,
                                    "unacknowledged prefix starts at rejected event");
                      sink->rejectSequence = 0;
                      const auto resumed   = adapter.drainOnce();
                      checks.expect(resumed.handedOff == 2 && resumed.status == AuditDrainStatus::Completed, "retry handles both pending events");
                      checks.expect(sink->records.size() == 3 && sink->records[0].sequence() == 1 && sink->records[1].sequence() == 2
                                        && sink->records[2].sequence() == 3,
                                    "all events reach the sink in order without overwriting");
                      checks.expect(adapter.healthSnapshot().takenUnacknowledged == 0, "retry clears failed hand-off state");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditIndependentProducerRings{
    "One failing audit stream does not prevent another stream from handing off",
    "unit",
    [] {
        return speclab::Test("audit-independent-rings")
            .Then("the failed stream stays queued while the other stream is acknowledged",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<1>          first{"device:boot:A"};
                      AuditRing<1>          second{"device:boot:B"};
                      AuditSinkAdapter      adapter;
                      auto                  sink = std::make_shared<RecordingAuditSink>();
                      sink->rejectStream         = "device:boot:A";
                      checks.expect(adapter.addRing(first) == AuditRingRegistration::Registered, "ring registered");
                      checks.expect(adapter.addRing(second) == AuditRingRegistration::Registered, "ring registered");
                      adapter.setSink(sink);
                      checks.expect(first.tryRecord(auditRequest()).wasAdmitted(), "first stream admitted");
                      checks.expect(second.tryRecord(auditRequest()).wasAdmitted(), "second stream admitted");
                      const auto partial = adapter.drainOnce();
                      checks.expect(partial.status == AuditDrainStatus::SinkRejected && partial.handedOff == 1,
                                    "failure in one ring does not stop the other ring");
                      checks.expect(adapter.healthSnapshot().pendingInRings == 1 && adapter.healthSnapshot().takenUnacknowledged == 1,
                                    "only failed stream remains pending");
                      checks.expect(sink->records.size() == 1 && sink->records[0].streamId() == "device:boot:B", "other stream was handed off");
                      sink->rejectStream = {};
                      checks.expect(adapter.drainOnce().handedOff == 1, "first stream resumes");
                      checks.expect(adapter.healthSnapshot().pendingInRings == 0 && adapter.healthSnapshot().takenUnacknowledged == 0,
                                    "all pending work is cleared");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditHealthVisibleAfterWorkerFailure{
    "Audit health exposes a worker-thread failure after producer admission",
    "unit",
    [] {
        return speclab::Test("audit-health-after-worker-failure")
            .Then("the producer observes failed hand-off without consulting diagnostic logging",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<1>          ring{"device:boot:producer"};
                      AuditSinkAdapter      adapter;
                      auto                  sink = std::make_shared<RecordingAuditSink>();
                      sink->mode                 = RecordingAuditSink::Mode::Throw;
                      checks.expect(adapter.addRing(ring) == AuditRingRegistration::Registered, "ring registered");
                      adapter.setSink(sink);
                      const auto admission = ring.tryRecord(auditRequest());
                      checks.expect(admission.wasAdmitted(), "producer sees only admission to memory");
                      mddlog::AuditDrainResult outcome;
                      std::jthread             worker([&] {
                          outcome = adapter.drainOnce();
                      });
                      worker.join();
                      const auto health = adapter.healthSnapshot();
                      checks.expect(outcome.status == AuditDrainStatus::SinkThrew, "consumer reports its later failure");
                      checks.expect(health.dispatchFailures == 1 && health.takenUnacknowledged == 1 && health.pendingInRings == 1,
                                    "producer-side observer sees the independent asynchronous signal");
                      checks.expect(admission.wasAdmitted(), "later failure does not retroactively change admission");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditStreamIdentitiesAreUnique{
    "The audit consumer refuses a ring whose stream identity is invalid or already registered",
    "unit",
    [] {
        return speclab::Test("audit-stream-identity-uniqueness")
            .Then("concurrent and recreated producers cannot share an identity, and refusal is observable",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<2>          producerA{"device:boot9:producerA"};
                      AuditRing<2>          producerB{"device:boot9:producerB"};
                      AuditRing<2>          sameAsA{"device:boot9:producerA"};
                      AuditRing<2>          invalid{"has spaces"};
                      AuditSinkAdapter      adapter;
                      auto                  sink = std::make_shared<RecordingAuditSink>();
                      adapter.setSink(sink);
                      checks.expect(adapter.addRing(producerA) == AuditRingRegistration::Registered
                                        && adapter.addRing(producerB) == AuditRingRegistration::Registered,
                                    "distinct identities are registered");
                      checks.expect(adapter.addRing(sameAsA) == AuditRingRegistration::DuplicateStream, "a concurrent producer reusing an identity is refused");
                      checks.expect(adapter.addRing(invalid) == AuditRingRegistration::InvalidStream, "an invalid identity is refused");
                      const auto refused = adapter.healthSnapshot();
                      checks.expect(refused.configurationErrors == 2 && refused.lastIssue == AuditDrainStatus::InvalidStream,
                                    "each refusal is a configuration error in audit health");

                      // Both registered streams start at sequence 1; their identities disambiguate them.
                      checks.expect(producerA.tryRecord(auditRequest()).sequence() == 1 && producerB.tryRecord(auditRequest()).sequence() == 1,
                                    "independent producers each emit sequence 1");
                      checks.expect(sameAsA.tryRecord(auditRequest()).wasAdmitted(), "a refused ring can still admit to its own memory");
                      checks.expect(adapter.drainOnce().handedOff == 2, "only registered rings are drained");
                      checks.expect(sink->records.size() == 2 && sink->records[0].streamId() != sink->records[1].streamId(),
                                    "(streamId, sequence) stays unambiguous after aggregation");
                      checks.expect(sameAsA.acknowledgedCount() == 0, "the refused ring's event is never handed off under a duplicate identity");
                      checks.raise();
                  })
            .And("a producer recreated with its old identity is refused once its counter restarts",
                 [] {
                     speclab::core::Checks checks;
                     AuditSinkAdapter      adapter;
                     auto                  sink = std::make_shared<RecordingAuditSink>();
                     adapter.setSink(sink);
                     auto first = std::make_unique<AuditRing<2>>("device:boot9:input");
                     checks.expect(adapter.addRing(*first) == AuditRingRegistration::Registered, "first instance registered");
                     checks.expect(first->tryRecord(auditRequest()).sequence() == 1, "first instance emits sequence 1");
                     checks.expect(adapter.drainOnce().handedOff == 1, "first instance drained");
                     AuditRing<2> recreated{"device:boot9:input"};
                     checks.expect(adapter.addRing(recreated) == AuditRingRegistration::DuplicateStream, "the restarted counter cannot reuse the identity");
                     AuditRing<2> renamed{"device:boot9:input2"};
                     checks.expect(adapter.addRing(renamed) == AuditRingRegistration::Registered, "a new producer instance identity is accepted");
                     checks.raise();
                 })
            .Execute();
    }};

}  // namespace
