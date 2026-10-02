/**
 * @brief TransportConsumer (ADR-003 Decision 5, issue #68): bounded, non-blocking producers,
 *        saturation, synchronous re-entrant logging, synchronous and asynchronous transport
 *        failure detachment, and sink-free health counters.
 */
import std;
import speclab;
import mddlog.core.ring;
import mddlog.core.auditevent;
import mddlog.core.auditring;
import mddlog.sinks.auditsink;
import mddlog.adapter.logrecord;
import mddlog.adapter.transportconsumer;

namespace {

// Browser transports (WebFront adoption, #72) sit on this consumer: no audit ring or audit
// sink can be registered, and a transport callback can only receive diagnostic records.
template <typename Ring>
concept ConsumesRing = requires(mddlog::adapter::TransportConsumer& consumer, Ring& ring) { consumer.addRing(ring); }
                       || requires(mddlog::adapter::TransportConsumer& consumer, Ring& ring) { consumer.addConsumerRing(ring); };
template <typename Callback>
concept RegistersTransport = requires(mddlog::adapter::TransportConsumer& consumer, Callback callback) { consumer.addTransport(callback); };

static_assert(ConsumesRing<mddlog::core::RingLog<4>>);
static_assert(!ConsumesRing<mddlog::core::AuditRing<4>>);
static_assert(!RegistersTransport<std::shared_ptr<mddlog::sinks::AuditSink>>);
static_assert(!RegistersTransport<std::function<void(const mddlog::core::AuditEvent&)>>);

using mddlog::adapter::TransportConsumer;
using mddlog::core::Admission;
using mddlog::core::LogLevel;
using mddlog::core::LogRecord;
using mddlog::core::RawTime;
using mddlog::core::RecordInput;
using mddlog::core::RingLog;

[[nodiscard]] RecordInput inputWith(std::string_view message,
                                    std::string_view component     = "transport",
                                    std::string_view operationId   = "operation",
                                    std::string_view correlationId = "correlation") noexcept {
    return {.level         = LogLevel::Info,
            .time          = RawTime::unavailable(),
            .location      = std::source_location::current(),
            .message       = message,
            .component     = component,
            .operationId   = operationId,
            .correlationId = correlationId};
}

/**
 * @brief Join @p thread within @p bound, or abort instead of hanging - same rationale as
 *        SinkRegistrySpec.cpp's joinWithinBoundOrAbort: a real deadlock must fail the process
 *        loudly rather than leave a detached thread touching destroyed test locals.
 */
void joinWithinBoundOrAbort(std::thread& thread, std::future<void>& completion, std::chrono::seconds bound, std::string_view what) {
    if (completion.wait_for(bound) == std::future_status::ready) {
        thread.join();
        return;
    }
    std::cerr << std::format("TransportConsumerSpec: {} did not complete within {}; aborting instead of hanging or detaching\n", what, bound);
    std::abort();
}

const speclab::Register slowTransportDoesNotBlockProducer{
    "TransportConsumer: a blocked transport never blocks a producer or discards its concurrent records",
    "unit",
    [] {
        return speclab::Test("transport-consumer-slow-transport-does-not-block-producer")
            .Then("a producer fills its ring while the transport waits, independently of reentrant suppression",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<4>            ring;
                      RingLog<4>            consumerRing;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);
                      consumer.addConsumerRing(consumerRing);
                      std::promise<void>         entered;
                      auto                       enteredFuture = entered.get_future();
                      std::promise<void>         release;
                      auto                       releaseFuture = release.get_future();
                      std::atomic<std::uint64_t> writes{0};
                      std::ignore = consumer.addTransport([&](const LogRecord&) {
                          if (writes.fetch_add(1, std::memory_order_relaxed) == 0) {
                              entered.set_value();
                              releaseFuture.wait();
                          }
                          std::ignore = consumerRing.tryWrite(inputWith("nested"));
                      });
                      checks.expect(ring.tryWrite(inputWith("first")).admission() == Admission::Written, "first record admitted");
                      std::promise<void> drained;
                      auto               drainedFuture = drained.get_future();
                      std::thread        worker([&] {
                          std::ignore = consumer.drainOnce();
                          drained.set_value();
                      });
                      if (enteredFuture.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
                          std::abort();

                      for (int i = 0; i < 4; ++i)
                          checks.expect(ring.tryWrite(inputWith("concurrent")).admission() == Admission::Written, "concurrent record admitted");
                      checks.expect(ring.tryWrite(inputWith("full")).admission() == Admission::Refused, "saturation refuses immediately");
                      checks.expect(drainedFuture.wait_for(std::chrono::seconds{0}) == std::future_status::timeout,
                                    "producer completes while transport is still blocked awaiting release");
                      release.set_value();
                      joinWithinBoundOrAbort(worker, drainedFuture, std::chrono::seconds{5}, "blocked transport drain");
                      checks.expect(consumer.drainOnce() == 4, "concurrent producer records survive suppression");
                      checks.expect(writes.load(std::memory_order_relaxed) == 5, "all five external records are delivered");
                      checks.expect(consumer.healthSnapshot().reentrantRecords == 5, "only consumer-thread records are suppressed");
                      checks.expect(consumer.healthSnapshot().ringRefusals == 1, "saturation stays observable");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register saturationRefusesImmediatelyWithCounter{
    "TransportConsumer: ring saturation refuses immediately and is observable without draining",
    "unit",
    [] {
        return speclab::Test("transport-consumer-saturation")
            .Then("a full ring refuses new writes and the refusal is visible in the health snapshot",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<2>            ring;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);

                      checks.expect(ring.tryWrite(inputWith("one")).admission() == Admission::Written, "first slot admitted");
                      checks.expect(ring.tryWrite(inputWith("two")).admission() == Admission::Written, "second slot admitted");
                      checks.expect(ring.tryWrite(inputWith("refused")).admission() == Admission::Refused, "third write refused: ring is full");
                      checks.expect(ring.tryWrite(inputWith("refused-again")).admission() == Admission::Refused,
                                    "saturation does not grow the ring or overwrite a published slot");

                      const auto snapshot = consumer.healthSnapshot();
                      checks.expect(snapshot.ringRefusals == 2, "both refusals are counted without requiring a drain");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register reentrantLoggingDoesNotRecurse{
    "TransportConsumer: unconditional reentrant logging terminates without feedback or recursive dispatch",
    "unit",
    [] {
        return speclab::Test("transport-consumer-reentrant-write-not-recursive")
            .Then("one external record causes one write even when every write logs and drains again",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<4>            producerRing;
                      RingLog<4>            consumerRing;
                      TransportConsumer     consumer;
                      consumer.addRing(producerRing);
                      consumer.addConsumerRing(consumerRing);

                      std::size_t writes = 0;
                      std::ignore        = consumer.addTransport([&](const LogRecord&) {
                          ++writes;
                          checks.expect(consumerRing.tryWrite(inputWith("nested")).admission() == Admission::Written,
                                        "the consumer thread can publish its nested record");
                          checks.expect(consumer.drainOnce() == 0, "nested drain does not invoke a transport");
                      });

                      checks.expect(producerRing.tryWrite(inputWith("outer")).admission() == Admission::Written, "outer record admitted");
                      checks.expect(consumer.drainOnce() == 1, "outer record dispatched");
                      for (int i = 0; i < 100; ++i)
                          checks.expect(consumer.drainOnce() == 0, "no feedback survives to the next drain");
                      checks.expect(writes == 1, "one input causes exactly one transport write");
                      checks.expect(consumer.healthSnapshot().reentrantRecords == 1, "suppression is observable without a sink");
                      checks.expect(consumerRing.drain().empty(), "nested record has been released");

                      checks.expect(consumerRing.tryWrite(inputWith("outside dispatch")).admission() == Admission::Written,
                                    "consumer-thread logging outside dispatch is admitted");
                      checks.expect(consumer.drainOnce() == 1, "consumer record outside dispatch is delivered");
                      checks.expect(writes == 2, "transport resumes normally after the guard clears");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register synchronousFailureDetachesBeforeFurtherEmission{
    "TransportConsumer: a synchronous transport throw detaches it before any further record reaches it",
    "unit",
    [] {
        return speclab::Test("transport-consumer-synchronous-failure-detaches")
            .Then("the failing transport never receives its own failure line or any record after it",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<4>            ring;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);

                      std::vector<std::string> received;
                      std::ignore = consumer.addTransport([&](const LogRecord& record) {
                          received.push_back(record.message);
                          throw std::runtime_error("transport write failed");
                      });

                      checks.expect(ring.tryWrite(inputWith("first")).admission() == Admission::Written, "first record admitted");
                      checks.expect(consumer.drainOnce() == 1, "first record is dispatched, causing the transport to throw");
                      checks.expect(received == std::vector<std::string>{"first"}, "the transport received exactly the record that made it fail");

                      const auto afterFailure = consumer.healthSnapshot();
                      checks.expect(afterFailure.writeFailures == 1, "the synchronous throw is counted");
                      checks.expect(afterFailure.detachments == 1, "the failure is reflected as a detachment");
                      checks.expect(afterFailure.activeTransports == 0, "the transport is no longer registered");

                      checks.expect(ring.tryWrite(inputWith("second")).admission() == Admission::Written, "a later record is still admitted to the ring");
                      checks.expect(consumer.drainOnce() == 1, "the ring still drains one record");
                      checks.expect(received == std::vector<std::string>{"first"},
                                    "the detached transport receives nothing further - not the failing record's own line, nor anything after it");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register ordinaryRemovalReclaimsRegistrations{
    "TransportConsumer: ordinary disconnection retires a transport without recording a failure",
    "unit",
    [] {
        return speclab::Test("transport-consumer-ordinary-removal")
            .Then("repeated add/remove cycles leave no live transport or failure count",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<4>            ring;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);

                      std::uint64_t             writes = 0;
                      TransportConsumer::Handle stale;
                      for (int i = 0; i < 128; ++i) {
                          const auto handle = consumer.addTransport([&](const LogRecord&) {
                              ++writes;
                          });
                          if (i == 0)
                              stale = handle;
                          consumer.removeTransport(handle);
                          checks.expect(consumer.healthSnapshot().activeTransports == 0, "each removed transport is inactive");
                      }

                      const auto current = consumer.addTransport([&](const LogRecord&) {
                          ++writes;
                      });
                      consumer.removeTransport(stale);
                      checks.expect(consumer.healthSnapshot().activeTransports == 1, "a stale handle cannot retire a later registration");
                      checks.expect(ring.tryWrite(inputWith("live")).admission() == Admission::Written, "record admitted");
                      checks.expect(consumer.drainOnce() == 1, "record dispatched");
                      checks.expect(writes == 1, "only the current transport receives the record");

                      consumer.removeTransport(current);
                      const auto after = consumer.healthSnapshot();
                      checks.expect(after.activeTransports == 0, "ordinary removal leaves no active transport");
                      checks.expect(after.reportedFailures == 0 && after.writeFailures == 0 && after.detachments == 0,
                                    "ordinary removal does not count as a failure");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register asynchronousFailureFromAnotherThreadDetachesWithoutAmplification{
    "TransportConsumer: a failure reported from another thread detaches the transport without amplification",
    "unit",
    [] {
        return speclab::Test("transport-consumer-asynchronous-failure-detaches")
            .Then("reportFailure() from a helper thread stops further invocations while drainOnce() keeps running",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<64>           ring;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);

                      std::atomic<std::uint64_t> invocations{0};
                      const auto                 handle = consumer.addTransport([&](const LogRecord&) {
                          invocations.fetch_add(1, std::memory_order_relaxed);
                      });

                      // Publish a first batch, drain it, and let the reporter's reportFailure() run
                      // concurrently with further admissions and a further drainOnce(): the property under
                      // test is that no NEW invocation starts once retirement has taken effect, not a strict
                      // ordering against records that were already in flight.
                      for (int i = 0; i < 8; ++i)
                          checks.expect(ring.tryWrite(inputWith("pre")).admission() == Admission::Written, "pre-failure record admitted");
                      checks.expect(consumer.drainOnce() == 8, "pre-failure batch is dispatched");
                      const auto invocationsBeforeReport = invocations.load(std::memory_order_relaxed);
                      checks.expect(invocationsBeforeReport == 8, "every pre-failure record reached the transport");

                      std::promise<void> completion;
                      auto               future = completion.get_future();
                      std::thread        reporter([&] {
                          consumer.reportFailure(handle);
                          completion.set_value();
                      });
                      joinWithinBoundOrAbort(reporter, future, std::chrono::seconds{5}, "reportFailure() from another thread");

                      const auto afterReport = consumer.healthSnapshot();
                      checks.expect(afterReport.reportedFailures == 1, "the asynchronous failure is counted");
                      checks.expect(afterReport.detachments == 1, "the failure is reflected as a detachment");
                      checks.expect(afterReport.activeTransports == 0, "the transport is no longer registered after reportFailure() returns");

                      for (int i = 0; i < 8; ++i)
                          checks.expect(ring.tryWrite(inputWith("post")).admission() == Admission::Written, "post-failure record admitted");
                      checks.expect(consumer.drainOnce() == 8, "the ring still drains, independent of the detached transport");
                      checks.expect(invocations.load(std::memory_order_relaxed) == invocationsBeforeReport,
                                    "no invocation happened after reportFailure() - total writes stayed bounded, no amplification");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register healthReadableWithoutAnySink{
    "TransportConsumer: health counters are readable without any sink involved",
    "unit",
    [] {
        return speclab::Test("transport-consumer-health-without-sink")
            .Then("healthSnapshot() reflects delivered, failed and active counts on a bare consumer",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<4>            ring;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);

                      const auto empty = consumer.healthSnapshot();
                      checks.expect(empty.delivered == 0 && empty.writeFailures == 0 && empty.reportedFailures == 0 && empty.detachments == 0,
                                    "a freshly constructed consumer reports zeroed counters");
                      checks.expect(empty.activeTransports == 0, "no transport is registered yet");

                      std::ignore = consumer.addTransport([](const LogRecord&) {});
                      checks.expect(ring.tryWrite(inputWith("ok")).admission() == Admission::Written, "record admitted");
                      checks.expect(consumer.drainOnce() == 1, "record dispatched");

                      const auto after = consumer.healthSnapshot();
                      checks.expect(after.delivered == 1, "a successful write is counted");
                      checks.expect(after.activeTransports == 1, "the transport remains registered after a successful write");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
