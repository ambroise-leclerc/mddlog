/**
 * @brief TransportConsumer (ADR-003 Decision 5, issue #68): bounded, non-blocking producers,
 *        saturation, synchronous re-entrant logging, synchronous and asynchronous transport
 *        failure detachment, and sink-free health counters.
 */
import std;
import speclab;
import mddlog.core.ring;
import mddlog.adapter.logrecord;
import mddlog.adapter.transportconsumer;

namespace {

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
    "TransportConsumer: a slow transport write never blocks a producer thread",
    "unit",
    [] {
        return speclab::Test("transport-consumer-slow-transport-does-not-block-producer")
            .Then("a bounded burst of tryWrite() calls completes quickly regardless of consumer speed",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<4>            ring;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);
                      std::ignore = consumer.addTransport([](const LogRecord&) {
                          std::this_thread::sleep_for(std::chrono::milliseconds{200});
                      });

                      const auto start = std::chrono::steady_clock::now();
                      for (int i = 0; i < 4; ++i)
                          checks.expect(ring.tryWrite(inputWith("burst")).admission() == Admission::Written, "each burst write is admitted");
                      const auto elapsed = std::chrono::steady_clock::now() - start;
                      checks.expect(elapsed < std::chrono::milliseconds{100}, "producer bursts complete without waiting on the (undrained) slow transport");
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
    "TransportConsumer: a log emitted from within a transport write is not delivered within the same drainOnce()",
    "unit",
    [] {
        return speclab::Test("transport-consumer-reentrant-write-not-recursive")
            .Then("the nested record is queued for the next drainOnce() instead of re-entering the transport",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<4>            ring;
                      TransportConsumer     consumer;
                      consumer.addRing(ring);

                      std::vector<std::string> writes;
                      std::size_t              invocationDepth = 0;
                      std::size_t              maxDepth        = 0;
                      std::ignore                              = consumer.addTransport([&](const LogRecord& record) {
                          ++invocationDepth;
                          maxDepth = std::max(maxDepth, invocationDepth);
                          writes.push_back(record.message);
                          if (record.message == "outer") {
                              // The Frame::addBuffer cycle: logging synchronously while this very write is
                              // in flight. This must land in the ring for a later drainOnce(), never re-enter
                              // this transport within the current dispatch.
                              checks.expect(ring.tryWrite(inputWith("nested")).admission() == Admission::Written,
                                            "the reentrant log is admitted to the ring like any other producer write");
                          }
                          --invocationDepth;
                      });

                      checks.expect(ring.tryWrite(inputWith("outer")).admission() == Admission::Written, "outer record is admitted");
                      checks.expect(consumer.drainOnce() == 1, "only the outer record is dispatched by this drainOnce()");
                      checks.expect(maxDepth == 1, "the transport was never invoked recursively");
                      checks.expect(writes == std::vector<std::string>{"outer"}, "the nested record was not delivered within the same call");

                      checks.expect(consumer.drainOnce() == 1, "the nested record is delivered on the next drainOnce()");
                      checks.expect(writes == (std::vector<std::string>{"outer", "nested"}), "the nested record is delivered exactly once, afterward");
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

                      std::ignore = consumer.addTransport([](const LogRecord&) { });
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
