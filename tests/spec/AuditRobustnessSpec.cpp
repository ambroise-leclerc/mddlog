/** @brief Reproducible admission model and concurrent audit delivery with permitted observers. */
import std;
import speclab;
import mddlog.adapter.auditdrain;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

#include "../framework/AuditReaderExercise.hpp"

namespace {
using namespace mddlog::adapter;
using mddlog::core::AuditRing;

const speclab::Register admissionModel{
    "AuditRing follows a generated queue model without consuming refused identities",
    "unit",
    [] {
        return speclab::Test("audit-generated-admission-model")
            .Then("32 seeds exercise saturation, invalid requests, partial acknowledgements and slot reuse",
                  [] {
                      for (unsigned seed = 1; seed <= 32; ++seed) {
                          std::mt19937                                      random{seed};
                          AuditRing<7>                                      ring{"model/producer"};
                          std::deque<std::pair<std::uint64_t, std::string>> expected;
                          std::uint64_t                                     admitted     = 0;
                          std::uint64_t                                     acknowledged = 0;
                          std::uint64_t                                     refused      = 0;
                          for (int step = 0; step < 2000; ++step) {
                              if (random() % 4 == 0) {
                                  const auto  view  = ring.drain();
                                  std::size_t index = 0;
                                  for (auto part : {view.first(), view.second()}) {
                                      for (const auto& event : part) {
                                          mddlog::spec::requireReaderInvariant(index < expected.size());
                                          const auto& item = expected.at(index++);
                                          mddlog::spec::requireReaderInvariant(event.sequence() == item.first && event.detail() == item.second);
                                      }
                                  }
                                  mddlog::spec::requireReaderInvariant(index == expected.size());
                                  const auto count = random() % (expected.size() + 1);
                                  mddlog::spec::requireReaderInvariant(!ring.acknowledge(view, expected.size() + 1));
                                  mddlog::spec::requireReaderInvariant(ring.acknowledge(view, count));
                                  acknowledged += count;
                                  for (std::size_t k = 0; k < count; ++k)
                                      expected.pop_front();
                              } else {
                                  const bool invalid = random() % 9 == 0;
                                  const auto detail  = std::to_string(seed) + ":" + std::to_string(step);
                                  const auto result  = ring.tryRecord({.action = invalid ? "bad action" : "model.action", .target = "test", .detail = detail});
                                  if (invalid) {
                                      mddlog::spec::requireReaderInvariant(result.refusal()
                                                                           && result.refusal()->reason == mddlog::core::AuditRefusalReason::InvalidIdentifier);
                                  } else if (expected.size() == 7) {
                                      ++refused;
                                      mddlog::spec::requireReaderInvariant(result.refusal()
                                                                           && result.refusal()->reason == mddlog::core::AuditRefusalReason::RingFull);
                                  } else {
                                      ++admitted;
                                      mddlog::spec::requireReaderInvariant(result.wasAdmitted() && result.sequence() == admitted);
                                      expected.emplace_back(admitted, detail);
                                  }
                              }
                              mddlog::spec::requireReaderInvariant(ring.admittedCount() == admitted && ring.acknowledgedCount() == acknowledged
                                                                   && ring.refusalCount() == refused);
                          }
                      }
                  })
            .Execute();
    }};

class RetryingSink final : public mddlog::sinks::AuditSink {
public:
    explicit RetryingSink(std::shared_ptr<PersistingAuditSink> target) : inner(std::move(target)) {}
    [[nodiscard]] bool accept(const mddlog::core::AuditEvent& event) override {
        auto&      previous     = lastAttempt[std::string{event.streamId()}];
        const bool firstAttempt = previous != event.sequence();
        previous                = event.sequence();
        if (firstAttempt && event.sequence() % 11 == 0) {
            ++throws;
            throw std::runtime_error("before hand-off");
        }
        if (firstAttempt && event.sequence() % 7 == 0) {
            ++rejections;
            return false;
        }
        return inner->accept(event);
    }
    std::size_t rejections = 0;
    std::size_t throws     = 0;

private:
    std::shared_ptr<PersistingAuditSink> inner;
    std::map<std::string, std::uint64_t> lastAttempt;
};

const speclab::Register concurrentAudit{
    "AuditRing concurrent producers survive retries while health and durable claims are observed",
    "unit",
    [] {
        return speclab::Test("audit-concurrent-retries-observers")
            .Then("three SPSC producers and one consumer retain all 1200 events without false durable claims",
                  [] {
                      speclab::core::Checks                            checks;
                      constexpr std::size_t                            count       = 3;
                      constexpr std::uint64_t                          perProducer = 400;
                      const std::array<std::string, count>             ids{"concurrent/a", "concurrent/b", "concurrent/c"};
                      std::array<std::unique_ptr<AuditRing<8>>, count> rings;
                      InMemoryStorageMedium                            medium{128};
                      InMemoryAnchorProvider                           provider{"concurrent/witness"};
                      StorageConfig                                    config;
                      config.segmentCount       = 128;
                      config.segmentSize        = 8192;
                      config.maxProducerStreams = count;
                      config.sync.recordBound   = 1;
                      config.provider           = &provider;
                      const auto made           = PersistingAuditSink::create(medium, config);
                      checks.expect(made.has_value(), "sink starts");
                      checks.raise();
                      auto             sink  = *made;
                      auto             retry = std::make_shared<RetryingSink>(sink);
                      AuditSinkAdapter adapter;
                      for (std::size_t k = 0; k < count; ++k) {
                          rings.at(k) = std::make_unique<AuditRing<8>>(ids.at(k));
                          checks.expect(adapter.addRing(*rings.at(k)) == AuditRingRegistration::Registered, "ring registered before observers start");
                      }
                      checks.raise();
                      adapter.setSink(retry);
                      const auto                       deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
                      std::latch                       filled{count};
                      std::latch                       beginDrain{1};
                      std::array<std::uint64_t, count> admitted{};
                      std::array<std::uint64_t, count> refusals{};
                      std::array<bool, count>          invalid{};
                      std::atomic<bool>                stopObserver{false};
                      bool                             observerFailed = false;
                      bool                             consumerFailed = false;
                      std::array<std::thread, count>   producers;
                      for (std::size_t k = 0; k < count; ++k) {
                          producers.at(k) = std::thread([&, k] {
                              auto& ring = *rings.at(k);
                              for (std::uint64_t n = 1; n <= 8; ++n) {
                                  const auto result = ring.tryRecord({.action = "load.action", .target = "test", .sourceSequence = n});
                                  invalid.at(k)     = invalid.at(k) || !result.wasAdmitted();
                              }
                              const auto full = ring.tryRecord({.action = "load.action", .target = "test", .sourceSequence = 9});
                              invalid.at(k)   = invalid.at(k) || !full.refusal() || full.refusal()->reason != mddlog::core::AuditRefusalReason::RingFull;
                              ++refusals.at(k);
                              admitted.at(k) = 8;
                              filled.count_down();
                              beginDrain.wait();
                              while (admitted.at(k) < perProducer && std::chrono::steady_clock::now() < deadline) {
                                  const auto result = ring.tryRecord({.action = "load.action", .target = "test", .sourceSequence = admitted.at(k) + 1});
                                  if (result.wasAdmitted())
                                      ++admitted.at(k);
                                  else if (result.refusal() && result.refusal()->reason == mddlog::core::AuditRefusalReason::RingFull)
                                      ++refusals.at(k);
                                  else
                                      invalid.at(k) = true;
                                  std::this_thread::yield();
                              }
                          });
                      }
                      std::thread observer([&] {
                          std::uint64_t previous = 0;
                          while (!stopObserver.load()) {
                              const auto health = adapter.healthSnapshot();
                              observerFailed    = observerFailed || health.handedOff < previous || health.handedOff > count * perProducer;
                              previous          = health.handedOff;
                              (void)sink->health();
                              for (const auto& id : ids) {
                                  const auto claim = sink->durableClaim(id);
                                  // Claims are sampled first: durablePosition can only advance afterwards.
                                  if (claim)
                                      observerFailed = observerFailed || claim->position > sink->durablePosition(id) || claim->position > perProducer;
                              }
                              std::this_thread::yield();
                          }
                      });
                      std::thread consumer([&] {
                          filled.wait();
                          beginDrain.count_down();
                          while (adapter.healthSnapshot().handedOff < count * perProducer && std::chrono::steady_clock::now() < deadline) {
                              const auto result = adapter.drainOnce();
                              consumerFailed    = consumerFailed
                                               || (result.status != AuditDrainStatus::Completed && result.status != AuditDrainStatus::SinkThrew
                                                   && result.status != AuditDrainStatus::SinkRejected);
                              std::this_thread::yield();
                          }
                          sink->flush();
                          for (const auto& id : ids)
                              (void)sink->advanceAnchor(id);
                      });
                      for (auto& producer : producers)
                          producer.join();
                      consumer.join();
                      stopObserver.store(true);
                      observer.join();
                      checks.expect(!observerFailed && !consumerFailed, "only permitted operations and monotone published claims");
                      checks.expect(retry->throws > 0 && retry->rejections > 0, "both rejection and exception paths exercised");
                      const auto health = adapter.healthSnapshot();
                      checks.expect(health.handedOff == count * perProducer && health.pendingInRings == 0 && health.takenUnacknowledged == 0,
                                    "all admitted events handed off once");
                      RetainedPosition retained;
                      LogVerifier      verifier{medium, provider, retained};
                      const auto       report = verifier.verify();
                      for (std::size_t k = 0; k < count; ++k) {
                          checks.expect(!invalid.at(k) && admitted.at(k) == perProducer && rings.at(k)->refusalCount() == refusals.at(k),
                                        "producer refusals match its own results");
                          const auto* stream = report.find(ids.at(k));
                          checks.expect(stream && stream->report.verdict == Verdict::Anchored && stream->report.anchoredThrough == perProducer,
                                        "reader verifies every producer through 400");
                      }
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
