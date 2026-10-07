/** @brief Host-driven lifecycle: fairness, idle confirmation, provider retry and explicit shutdown. */
import std;
import mddlog.adapter.auditservice;
import speclab;

namespace {
using namespace mddlog::adapter;
using mddlog::core::AuditRing;
using namespace std::chrono_literals;

StorageConfig storageConfig() {
    StorageConfig config;
    config.segmentSize        = 2048;
    config.segmentCount       = 24;
    config.maxProducerStreams = 2;
    return config;
}

mddlog::core::AuditInput request() {
    return {.action = "inventory.inspect", .target = "warehouse"};
}

const speclab::Register configuration{"Audit service rejects invalid configuration before storage I/O", "unit", [] {
                                          return speclab::Test("audit-service-configuration")
                                              .Then("invalid budget, period and storage are distinguished",
                                                    [] {
                                                        speclab::core::Checks checks;
                                                        InMemoryStorageMedium medium{24};
                                                        auto                  config = AuditServiceConfig{.storage = storageConfig(), .maxRecordsPerRing = 0};
                                                        auto                  made   = AuditService::create(medium, config);
                                                        checks.expect(!made && made.error().issue == AuditServiceConfigError::InvalidRecordBudget,
                                                                      "zero budget rejected");
                                                        config.maxRecordsPerRing = 1;
                                                        config.anchorPeriod      = 0ns;
                                                        made                     = AuditService::create(medium, config);
                                                        checks.expect(!made && made.error().issue == AuditServiceConfigError::InvalidAnchorPeriod,
                                                                      "zero period rejected");
                                                        config.anchorPeriod        = 1s;
                                                        config.storage.segmentSize = 0;
                                                        made                       = AuditService::create(medium, config);
                                                        checks.expect(!made && made.error().storage == StorageConfigError::SegmentTooSmall,
                                                                      "storage cause retained");
                                                        checks.expect(medium.calls(InMemoryStorageMedium::Operation::Open) == 0, "no storage mutation");
                                                        checks.raise();
                                                    })
                                              .Execute();
                                      }};

const speclab::Register fairness{"Every ring receives its budget and shutdown preserves backlog", "unit", [] {
                                     return speclab::Test("audit-service-fair-stop")
                                         .Then("a bounded stop can be retried without closing pending streams",
                                               [] {
                                                   speclab::core::Checks checks;
                                                   InMemoryStorageMedium medium{24};
                                                   AuditRing<4>          first{"first"};
                                                   AuditRing<4>          second{"second"};
                                                   auto                  made = AuditService::create(medium, {.storage = storageConfig()});
                                                   checks.expect(made.has_value(), "service created");
                                                   checks.raise();
                                                   auto& service = **made;
                                                   checks.expect(service.addRing(first) == AuditServiceRegistration::Registered, "first registered");
                                                   checks.expect(service.addRing(second) == AuditServiceRegistration::Registered, "second registered");
                                                   checks.expect(service.addRing(first) == AuditServiceRegistration::DuplicateStream, "duplicate refused");
                                                   for (int count = 0; count < 4; ++count) {
                                                       checks.expect(first.tryRecord(request()).wasAdmitted(), "first admitted");
                                                       checks.expect(second.tryRecord(request()).wasAdmitted(), "second admitted");
                                                   }
                                                   checks.expect(service.poll().handedOff == 2, "one event per ring");
                                                   const auto before = service.health();
                                                   checks.expect(before.delivery.pendingInRings == 6 && before.delivery.handedOff == 2, "backlog observable");
                                                   checks.expect(service.addRing(first) == AuditServiceRegistration::Started, "registration frozen");
                                                   const auto partial = service.stop(1);
                                                   checks.expect(!partial.closed && partial.health.delivery.pendingInRings == 4,
                                                                 "bounded stop leaves sink open");
                                                   const auto ended = service.stop(2);
                                                   checks.expect(ended.closed && ended.health.delivery.pendingInRings == 0, "retry drains remaining events");
                                                   checks.expect(ended.health.delivery.handedOff == 8 && ended.health.delivery.reportedLosses == 0,
                                                                 "no silent loss");
                                                   const auto repeated = service.stop();
                                                   checks.expect(repeated.closed && repeated.health.delivery.handedOff == 8, "stop idempotent");
                                                   checks.expect(service.poll().handedOff == 0, "closed poll has no dispatch");
                                                   checks.raise();
                                               })
                                         .Execute();
                                 }};

const speclab::Register idle{"Inactive streams reach their sync age and unavailable providers are retried", "unit", [] {
                                 return speclab::Test("audit-service-idle-sync-anchor")
                                     .Then("only confirmed positions are anchored on a later idle poll",
                                           [] {
                                               speclab::core::Checks  checks;
                                               InMemoryStorageMedium  medium{24};
                                               InMemoryAnchorProvider provider{"witness"};
                                               auto                   now    = std::chrono::steady_clock::time_point{};
                                               auto                   config = storageConfig();
                                               config.clock                  = [&now] {
                                                   return now;
                                               };
                                               config.sync     = {.recordBound = 100, .ageBound = 10ms};
                                               config.provider = &provider;
                                               AuditRing<4> ring{"idle"};
                                               auto         made = AuditService::create(medium, {.storage = config, .anchorPeriod = 10ms});
                                               checks.expect(made.has_value(), "service created");
                                               checks.raise();
                                               auto& service = **made;
                                               checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered, "registered");
                                               checks.expect(ring.tryRecord(request()).wasAdmitted(), "admitted");
                                               checks.expect(service.poll().handedOff == 1, "handed off");
                                               checks.expect(service.health().storage.streams.at(0).durablePosition == 0, "admission is not confirmation");
                                               checks.expect(std::holds_alternative<AnchorAbsent>(provider.latest("idle")), "no premature anchor");
                                               provider.setAvailable(false);
                                               now += 10ms;
                                               checks.expect(service.poll().handedOff == 0, "idle host poll");
                                               const auto confirmed = service.health();
                                               checks.expect(confirmed.storage.streams.at(0).durablePosition == 1, "idle age sync");
                                               checks.expect(confirmed.storage.counters.anchorsUnavailable == 1, "unavailability visible");
                                               for (int cycle = 0; cycle < 8; ++cycle) {
                                                   now += 10ms;
                                                   (void)service.poll();
                                               }
                                               checks.expect(service.health().unanchored == 1 && service.health().delivery.reportedLosses == 0,
                                                             "prolonged outage preserves confirmed exposure");
                                               provider.setAvailable(true);
                                               (void)service.poll();
                                               checks.expect(std::holds_alternative<AnchorAbsent>(provider.latest("idle")), "retry respects period");
                                               now += 10ms;
                                               (void)service.poll();
                                               const auto anchored = provider.latest("idle");
                                               checks.expect(std::holds_alternative<Anchor>(anchored) && std::get<Anchor>(anchored).position == 1,
                                                             "confirmed position anchored after recovery");
                                               checks.expect(service.stop().closed, "stopped");
                                               checks.raise();
                                           })
                                     .Execute();
                             }};

const speclab::Register unsupported{"An unqualified medium never produces an anchor", "unit", [] {
                                        return speclab::Test("audit-service-unqualified")
                                            .Then("shutdown reports written records and zero confirmed position",
                                                  [] {
                                                      speclab::core::Checks  checks;
                                                      InMemoryStorageMedium  medium{24, false};
                                                      InMemoryAnchorProvider provider{"witness"};
                                                      auto                   config = storageConfig();
                                                      config.provider               = &provider;
                                                      AuditRing<1> ring{"unqualified"};
                                                      auto         made = AuditService::create(medium, {.storage = config});
                                                      checks.expect(made.has_value(), "service created");
                                                      checks.raise();
                                                      auto& service = **made;
                                                      checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered, "registered");
                                                      checks.expect(ring.tryRecord(request()).wasAdmitted(), "admitted");
                                                      const auto result = service.stop();
                                                      checks.expect(result.closed && result.health.delivery.handedOff == 1, "lifecycle ended");
                                                      checks.expect(result.health.storage.streams.at(0).appendedPosition == 1
                                                                        && result.health.storage.streams.at(0).durablePosition == 0,
                                                                    "unconfirmed record remains explicit");
                                                      checks.expect(result.health.storage.counters.syncUnsupported > 0, "unsupported observable");
                                                      checks.expect(std::holds_alternative<AnchorAbsent>(provider.latest("unqualified")), "no anchor offered");
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};

const speclab::Register failure{
    "Storage failure reaches the service health and the host callback",
    "unit",
    [] {
        return speclab::Test("audit-service-loss-signal")
            .Then("failed sync does not promote hand-off to durable confirmation",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{24};
                      medium.inject({.operation = InMemoryStorageMedium::Operation::Sync, .ordinal = 1, .effect = InMemoryStorageMedium::Effect::Fail});
                      std::uint64_t hostLosses = 0;
                      auto          config     = storageConfig();
                      config.reportLoss        = [&hostLosses](std::uint64_t count) {
                          hostLosses += count;
                      };
                      AuditRing<2> ring{"failure"};
                      auto         made = AuditService::create(medium, {.storage = config});
                      checks.expect(made.has_value(), "service created");
                      checks.raise();
                      auto& service = **made;
                      checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered, "registered");
                      checks.expect(ring.tryRecord(request()).wasAdmitted(), "first admitted");
                      checks.expect(service.poll().handedOff == 1, "failed sync follows hand-off");
                      const auto health = service.health();
                      checks.expect(health.delivery.reportedLosses == 1 && hostLosses == 1, "both channels report loss");
                      checks.expect(health.storage.streams.at(0).durablePosition == 0 && health.storage.counters.syncFailures == 1,
                                    "failure visible without confirmation");
                      checks.expect(ring.tryRecord(request()).wasAdmitted(), "next event admitted to ring");
                      const auto stopped = service.stop();
                      checks.expect(!stopped.closed && stopped.health.delivery.pendingInRings == 1, "failed stream backlog retained");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register observers{
    "Service health observers can run alongside producer and consumer",
    "unit",
    [] {
        return speclab::Test("audit-service-concurrent-health")
            .Then("all admitted events are delivered and snapshots never claim more than appended",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{24};
                      AuditRing<16>         ring{"concurrent"};
                      auto                  made = AuditService::create(medium, {.storage = storageConfig()});
                      checks.expect(made.has_value(), "service created");
                      checks.raise();
                      auto& service = **made;
                      checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered, "registered before observers");
                      checks.raise();
                      constexpr std::uint64_t total = 64;
                      std::atomic<bool>       producerDone{false};
                      std::atomic<bool>       cancelProducer{false};
                      std::atomic<bool>       observing{true};
                      std::atomic<bool>       invalidSnapshot{false};
                      std::thread             producer([&] {
                          for (std::uint64_t admitted = 0; admitted < total && !cancelProducer.load(std::memory_order_relaxed);) {
                              if (ring.tryRecord(request()).wasAdmitted())
                                  ++admitted;
                              else
                                  std::this_thread::yield();
                          }
                          producerDone.store(true, std::memory_order_release);
                      });
                      std::thread             observer([&] {
                          while (observing.load(std::memory_order_acquire)) {
                              const auto health = service.health();
                              for (const auto& stream : health.storage.streams) {
                                  if (stream.durablePosition > stream.appendedPosition)
                                      invalidSnapshot.store(true, std::memory_order_relaxed);
                              }
                              std::this_thread::yield();
                          }
                      });
                      const auto              deadline = std::chrono::steady_clock::now() + 5s;
                      while ((!producerDone.load(std::memory_order_acquire) || service.health().delivery.pendingInRings != 0)
                             && std::chrono::steady_clock::now() < deadline) {
                          (void)service.poll();
                          std::this_thread::yield();
                      }
                      cancelProducer.store(true, std::memory_order_relaxed);
                      producer.join();
                      const auto stopped = service.stop();
                      observing.store(false, std::memory_order_release);
                      observer.join();
                      checks.expect(stopped.closed && stopped.health.delivery.handedOff == total, "all events delivered before stop");
                      checks.expect(stopped.health.storage.streams.at(0).durablePosition == total, "eligible double confirms every event");
                      checks.expect(stopped.health.delivery.reportedLosses == 0 && !invalidSnapshot.load(std::memory_order_relaxed),
                                    "health keeps confirmation bounded");
                      checks.raise();
                  })
            .Execute();
    }};

class LostAnswerProvider final : public AnchorProvider {
public:
    InMemoryAnchorProvider authority{"independent"};
    bool                   loseAnswer = true;
    std::size_t            advances   = 0;
    std::function<void()>  beforeAdvance;
    AdvanceAnswer          advance(const AnchorClaim& claim) override {
        if (beforeAdvance)
            beforeAdvance();
        ++advances;
        auto result = authority.advance(claim);
        return std::exchange(loseAnswer, false) ? AdvanceAnswer{ProviderUnavailable{}} : result;
    }
    RetireAnswer retire(std::string_view identity, std::uint64_t position) override {
        return authority.retire(identity, position);
    }
    LatestAnswer latest(std::string_view identity) override {
        return authority.latest(identity);
    }
    StreamsAnswer streams() override {
        return authority.streams();
    }
};

const speclab::Register globalBudget{
    "Service global budget rotates between producers including rejected attempts",
    "unit",
    [] {
        return speclab::Test("audit-service-global-budget")
            .Then("one attempt does not starve the next ring",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{24};
                      AuditRing<1>          first{"first"};
                      AuditRing<1>          second{"second"};
                      AuditRing<1>          excess{"third"};
                      LostAnswerProvider    provider;
                      auto                  config = storageConfig();
                      config.provider              = &provider;
                      auto made                    = AuditService::create(medium, {.storage = config, .maxAttemptsPerPoll = 1, .maxAnchorStreamsPerPoll = 1});
                      checks.expect(made.has_value(), "created");
                      checks.raise();
                      auto& service = **made;
                      checks.expect(service.addRing(first) == AuditServiceRegistration::Registered, "first");
                      checks.expect(service.addRing(second) == AuditServiceRegistration::Registered, "second");
                      checks.expect(service.addRing(excess) == AuditServiceRegistration::Capacity, "bounded enrollment");
                      checks.expect(first.tryRecord(request()).wasAdmitted() && second.tryRecord(request()).wasAdmitted(), "admitted");
                      checks.expect(!first.tryRecord(request()).wasAdmitted(), "saturation visible");
                      const auto one = service.poll();
                      checks.expect(one.attempted == 1 && one.handedOff == 1, "global cap");
                      checks.expect(service.poll().handedOff == 1 && second.acknowledgedCount() == 1, "rotation");
                      checks.expect(service.health().storage.streams.at(1).anchoredPosition == 1 && service.health().unanchored == 1,
                                    "uncertain first anchor does not starve the next stream");
                      const auto health = service.health();
                      checks.expect(health.delivery.admitted == 2 && health.delivery.ringFullRefusals == 1, "independent admission counters");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register reconciliation{
    "Service reconciles a lost witness answer without replaying the accepted claim",
    "unit",
    [] {
        return speclab::Test("audit-service-anchor-reconciliation")
            .Then("latest resolves uncertainty and a new claim advances normally",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{24};
                      LostAnswerProvider    provider;
                      auto                  now    = std::chrono::steady_clock::time_point{};
                      auto                  config = storageConfig();
                      config.provider              = &provider;
                      config.clock                 = [&now] {
                          return now;
                      };
                      AuditRing<2> ring{"lost-answer"};
                      auto         made = AuditService::create(medium, {.storage = config, .anchorPeriod = 10ms});
                      checks.expect(made.has_value(), "created");
                      checks.raise();
                      auto& service = **made;
                      checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered && ring.tryRecord(request()).wasAdmitted(),
                                    "registered and admitted");
                      (void)service.poll();
                      checks.expect(service.health().unanchored == 1, "uncertain answer remains exposed");
                      now += 10ms;
                      (void)service.poll();
                      checks.expect(provider.advances == 1 && service.health().unanchored == 0, "authenticated reconciliation without replay");
                      checks.expect(service.health().storage.integrity.empty(), "no false divergence");
                      checks.expect(ring.tryRecord(request()).wasAdmitted(), "new event");
                      now += 10ms;
                      (void)service.poll();
                      checks.expect(provider.advances == 2 && service.health().storage.streams.at(0).anchoredPosition == 2, "new claim accepted");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register ageAnchor{
    "Service anchors below its count threshold when an idle age expires",
    "unit",
    [] {
        return speclab::Test("audit-service-anchor-age")
            .Then("exposure is published until the configured age",
                  [] {
                      speclab::core::Checks  checks;
                      InMemoryStorageMedium  medium{24};
                      InMemoryAnchorProvider provider{"witness"};
                      auto                   now    = std::chrono::steady_clock::time_point{};
                      auto                   config = storageConfig();
                      config.provider               = &provider;
                      config.clock                  = [&now] {
                          return now;
                      };
                      AuditRing<1> ring{"age"};
                      auto         made = AuditService::create(medium, {.storage = config, .anchorRecordBound = 10, .anchorAgeBound = 20ms});
                      checks.expect(made.has_value(), "created");
                      checks.raise();
                      auto& service = **made;
                      checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered && ring.tryRecord(request()).wasAdmitted(), "enrolled");
                      (void)service.poll();
                      checks.expect(service.health().unanchored == 1 && service.health().storage.streams.at(0).unanchoredSince.has_value(),
                                    "window starts at confirmation");
                      now += 20ms;
                      (void)service.poll();
                      checks.expect(service.health().unanchored == 0 && !service.health().storage.streams.at(0).unanchoredSince, "idle age accepted");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register deadlines{"Service shutdown reports a slow operation and preserves rejected backlog", "unit", [] {
                                      return speclab::Test("audit-service-stop-deadline")
                                          .Then("a soft deadline cannot cancel a synchronous phase",
                                                [] {
                                                    speclab::core::Checks checks;
                                                    InMemoryStorageMedium medium{24};
                                                    auto                  now = std::chrono::steady_clock::time_point{};
                                                    LostAnswerProvider    provider;
                                                    provider.loseAnswer    = false;
                                                    provider.beforeAdvance = [&now] {
                                                        now += 10ms;
                                                    };
                                                    auto config     = storageConfig();
                                                    config.provider = &provider;
                                                    config.clock    = [&now] {
                                                        return now;
                                                    };
                                                    AuditRing<1> ring{"deadline"};
                                                    auto         made = AuditService::create(medium, {.storage = config});
                                                    checks.expect(made.has_value(), "created");
                                                    checks.raise();
                                                    auto& service = **made;
                                                    checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered
                                                                      && ring.tryRecord(request()).wasAdmitted(),
                                                                  "enrolled");
                                                    const auto stopped = service.stop(AuditStopOptions{.maxDrainPasses = 1, .timeBudget = 1ms});
                                                    checks.expect(!stopped.closed && stopped.status == AuditStopStatus::DeadlineExceeded
                                                                      && stopped.health.delivery.pendingInRings == 0 && stopped.health.delivery.handedOff == 1,
                                                                  "slow call overruns deadline without losing the handed-off event");
                                                    checks.expect(service.stop().closed, "unbounded-time retry closes");
                                                    checks.raise();
                                                })
                                          .Execute();
                                  }};


const speclab::Register startupFailure{
    "Service rejects failed ledger startup and undeclared retention",
    "unit",
    [] {
        return speclab::Test("audit-service-startup-failure")
            .Then("startup causes are readable before producers are bound",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{24};
                      auto                  config = storageConfig();
                      auto                  made   = AuditService::create(medium, {.storage = config, .capacityPolicy = AuditCapacityPolicy::RelieveDeclared});
                      checks.expect(!made && made.error().issue == AuditServiceConfigError::InvalidRetention, "no implicit retention");
                      config.ledger = LedgerConfig{.streamId = "ledger/failed-boot", .time = {}};
                      medium.inject({.operation = InMemoryStorageMedium::Operation::Sync, .ordinal = 1, .effect = InMemoryStorageMedium::Effect::Fail});
                      made = AuditService::create(medium, {.storage = config});
                      checks.expect(!made && made.error().issue == AuditServiceConfigError::StartupFailed
                                        && made.error().startupCause == StorageIssue::SyncFailed,
                                    "failed ledger distinguished from bad parameters");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register callbackFailure{
    "Service isolates callback failure and requires fresh enrollment after storage repair",
    "unit",
    [] {
        return speclab::Test("audit-service-callback-recovery")
            .Then("health survives a throwing callback and failed streams never resume",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{24};
                      medium.inject({.operation = InMemoryStorageMedium::Operation::Sync, .ordinal = 1, .effect = InMemoryStorageMedium::Effect::Fail});
                      AuditRing<2> ring{"before-repair"};
                      auto         config = storageConfig();
                      config.reportLoss   = [](std::uint64_t) {
                          throw std::runtime_error{"host callback"};
                      };
                      auto made = AuditService::create(medium, {.storage = config});
                      checks.expect(made.has_value(), "created");
                      checks.raise();
                      auto& service = **made;
                      checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered && ring.tryRecord(request()).wasAdmitted(), "enrolled");
                      checks.expect(service.poll().handedOff == 1, "callback cannot turn append into ring retry");
                      checks.expect(service.health().callbackFailures == 1 && service.health().delivery.reportedLosses == 1, "loss independent of callback");
                      checks.expect(ring.tryRecord(request()).wasAdmitted(), "next queued");
                      for (int count = 0; count < 8; ++count)
                          (void)service.poll();
                      checks.expect(service.health().delivery.pendingInRings == 1 && !service.stop().closed,
                                    "failed identity stays failed after fault disappears");
                      AuditRing<1> fresh{"after-repair"};
                      auto         repaired = AuditService::create(medium, {.storage = storageConfig()});
                      checks.expect(repaired.has_value(), "host explicitly recomposes on repaired medium");
                      checks.raise();
                      checks.expect((*repaired)->addRing(fresh) == AuditServiceRegistration::Registered && fresh.tryRecord(request()).wasAdmitted(),
                                    "fresh enrollment");
                      checks.expect((*repaired)->stop().health.unconfirmed == 0 && service.health().delivery.pendingInRings == 1,
                                    "new service cannot erase old backlog");
                      checks.raise();
                  })
            .Execute();
    }};


const speclab::Register declaredRetention{
    "Service relieves full storage only under explicit retention permissions",
    "integration",
    [] {
        return speclab::Test("audit-service-declared-retention")
            .Then("default capacity keeps backlog while declared rotation resumes it",
                  [] {
                      speclab::core::Checks checks;
                      for (const auto policy : {AuditCapacityPolicy::KeepPending, AuditCapacityPolicy::RelieveDeclared}) {
                          InMemoryStorageMedium  medium{24};
                          InMemoryAnchorProvider provider{"witness"};
                          auto                   now    = std::chrono::steady_clock::time_point{};
                          auto                   config = storageConfig();
                          config.ledger                 = LedgerConfig{.streamId = "ledger/session", .time = {}};
                          config.retention.rotate       = true;
                          config.provider               = &provider;
                          config.clock                  = [&now] {
                              return now;
                          };
                          AuditRing<1> ring{"capacity"};
                          auto         made = AuditService::create(medium, {.storage = config, .capacityPolicy = policy, .anchorPeriod = 1ns});
                          checks.expect(made.has_value(), "created");
                          checks.raise();
                          auto& service = **made;
                          checks.expect(service.addRing(ring) == AuditServiceRegistration::Registered, "enrolled");
                          bool full = false;
                          for (std::size_t count = 0; count < 1000 && !full; ++count) {
                              checks.expect(ring.tryRecord(request()).wasAdmitted(), "queued before capacity failure");
                              now += 1ns;
                              full = service.poll().handedOff == 0;
                          }
                          checks.expect(full && service.health().delivery.pendingInRings == 1, "full storage preserves event");
                          now               += 1ns;
                          const auto retried = service.poll();
                          const auto health  = service.health();
                          if (policy == AuditCapacityPolicy::KeepPending) {
                              checks.expect(retried.handedOff == 0 && health.storage.counters.segmentsReclaimed == 0,
                                            "standing storage permission alone does not enable service retention");
                          } else {
                              checks.expect(retried.handedOff == 1 && health.storage.counters.trimsRecorded > 0
                                                && health.storage.counters.segmentsReclaimed > 0,
                                            "durable trim permits resumption");
                          }
                          checks.expect(health.delivery.reportedLosses == 0, "no unreported loss");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register failedFairness{
    "Service global budget counts rejected attempts without starving healthy producers",
    "unit",
    [] {
        return speclab::Test("audit-service-rejected-fairness")
            .Then("a failed first stream cannot monopolize the global budget",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{24};
                      medium.inject({.operation = InMemoryStorageMedium::Operation::Sync, .ordinal = 1, .effect = InMemoryStorageMedium::Effect::Fail});
                      AuditRing<2> failed{"failed"};
                      AuditRing<2> healthy{"healthy"};
                      auto         made = AuditService::create(medium, {.storage = storageConfig(), .maxAttemptsPerPoll = 1});
                      checks.expect(made.has_value(), "created");
                      checks.raise();
                      auto& service = **made;
                      checks.expect(service.addRing(failed) == AuditServiceRegistration::Registered
                                        && service.addRing(healthy) == AuditServiceRegistration::Registered,
                                    "enrolled");
                      checks.expect(failed.tryRecord(request()).wasAdmitted(), "first queued");
                      (void)service.poll();
                      checks.expect(failed.tryRecord(request()).wasAdmitted() && healthy.tryRecord(request()).wasAdmitted(), "both queue after first failure");
                      const auto healthyTurn = service.poll();
                      checks.expect(healthyTurn.attempted == 1 && healthyTurn.handedOff == 1 && healthy.acknowledgedCount() == 1, "healthy turn");
                      const auto failedTurn = service.poll();
                      checks.expect(failedTurn.attempted == 1 && failedTurn.handedOff == 0 && failed.admittedCount() - failed.acknowledgedCount() == 1,
                                    "rejection consumes attempt without acknowledging");
                      checks.expect(healthy.tryRecord(request()).wasAdmitted() && service.poll().handedOff == 1, "healthy next turn");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
