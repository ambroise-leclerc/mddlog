/**
 * @brief Concurrent SPSC ring publication, reuse and per-ring ordering (issue #37).
 */
import std;
import speclab;
import mddlog.core.ring;

namespace {

using mddlog::core::Admission;
using mddlog::core::RawTime;
using mddlog::core::RefusalReason;
using mddlog::core::RingLog;

[[nodiscard]] mddlog::core::RecordInput numberedInput(std::string_view number) noexcept {
    return {.time = RawTime::unavailable(), .message = number, .component = "concurrency", .operationId = "ring", .correlationId = "test"};
}

const speclab::Register saturatedRingResumesAcrossThreads{
    "RingLog releases a saturated slot before the producer reuses it across threads",
    "unit",
    [] {
        return speclab::Test("ring-spsc-saturation-reuse")
            .Then("latches make saturation, drain, and resumed publication observable in order",
                  [] {
                      speclab::core::Checks    checks;
                      RingLog<2>               ring;
                      std::latch               start{2};
                      std::latch               filled{1};
                      std::latch               released{1};
                      std::latch               resumed{1};
                      bool                     firstWritten  = false;
                      bool                     secondWritten = false;
                      bool                     fullRefused   = false;
                      bool                     resumedWrite  = false;
                      bool                     firstAck      = false;
                      bool                     secondAck     = false;
                      std::vector<std::string> received;

                      std::thread producer([&] {
                          start.count_down();
                          start.wait();
                          firstWritten    = ring.tryWrite(numberedInput("0")).admission() == Admission::Written;
                          secondWritten   = ring.tryWrite(numberedInput("1")).admission() == Admission::Written;
                          const auto full = ring.tryWrite(numberedInput("2"));
                          fullRefused     = full.admission() == Admission::Refused && full.refusal()->reason == RefusalReason::RingFull;
                          filled.count_down();
                          released.wait();
                          resumedWrite = ring.tryWrite(numberedInput("2")).admission() == Admission::Written;
                          resumed.count_down();
                      });
                      std::thread consumer([&] {
                          start.count_down();
                          start.wait();
                          filled.wait();
                          const auto initial = ring.drain();
                          for (const auto& record : initial.first())
                              received.emplace_back(record.message());
                          for (const auto& record : initial.second())
                              received.emplace_back(record.message());
                          firstAck = ring.acknowledge(initial, initial.size());
                          released.count_down();
                          resumed.wait();
                          const auto later = ring.drain();
                          for (const auto& record : later.first())
                              received.emplace_back(record.message());
                          for (const auto& record : later.second())
                              received.emplace_back(record.message());
                          secondAck = ring.acknowledge(later, later.size());
                      });
                      producer.join();
                      consumer.join();

                      checks.expect(firstWritten && secondWritten && fullRefused && resumedWrite,
                                    "producer sees two admissions, RingFull, then a resumed admission");
                      checks.expect(firstAck && secondAck, "consumer acknowledges before and after reuse");
                      checks.expect(received == std::vector<std::string>{"0", "1", "2"}, "accepted values are read once and in order");
                      checks.expect(ring.refusalCount() == 1, "the controlled saturation increments the atomic counter once");
                      checks.expect(ring.writeSequence() == 3 && ring.readSequence() == 3, "both cursors reflect three accepted records");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register multipleRingsPreserveLocalOrderUnderLoad{
    "Independent RingLog producers preserve per-ring order while one consumer aggregates",
    "unit",
    [] {
        return speclab::Test("ring-multiple-producers-per-ring-order")
            .Then("three bounded SPSC rings preserve admitted values without loss, duplication, or overwrite",
                  [] {
                      speclab::core::Checks                           checks;
                      constexpr std::size_t                           ringCount          = 3;
                      constexpr int                                   recordsPerProducer = 500;
                      constexpr int                                   initialFill        = 4;
                      std::array<RingLog<4>, ringCount>               rings;
                      std::array<std::thread, ringCount>              producers;
                      std::array<int, ringCount>                      accepted{};
                      std::array<std::uint64_t, ringCount>            fullRefusals{};
                      std::array<bool, ringCount>                     unexpectedRefusal{};
                      std::array<std::vector<std::string>, ringCount> received;
                      std::latch                                      start{ringCount + 2};
                      std::latch                                      monitorReady{1};
                      std::latch                                      initialFull{ringCount};
                      std::latch                                      beginDrain{1};
                      std::atomic<bool>                               stopMonitor{false};
                      bool                                            acknowledgementFailed = false;
                      bool                                            counterRegressed      = false;
                      const auto                                      deadline              = std::chrono::steady_clock::now() + std::chrono::seconds{30};

                      for (std::size_t index = 0; index < ringCount; ++index) {
                          producers.at(index) = std::thread([&, index] {
                              start.count_down();
                              start.wait();
                              monitorReady.wait();
                              for (int first = 0; first < initialFill; ++first) {
                                  const auto message = std::to_string(first);
                                  if (rings.at(index).tryWrite(numberedInput(message)).admission() != Admission::Written)
                                      unexpectedRefusal.at(index) = true;
                              }
                              const auto saturated = rings.at(index).tryWrite(numberedInput("retry"));
                              if (saturated.admission() == Admission::Refused && saturated.refusal()->reason == RefusalReason::RingFull)
                                  ++fullRefusals.at(index);
                              else
                                  unexpectedRefusal.at(index) = true;
                              initialFull.count_down();
                              beginDrain.wait();
                              int sequence = initialFill;
                              while (sequence < recordsPerProducer && std::chrono::steady_clock::now() < deadline) {
                                  const auto message = std::to_string(sequence);
                                  const auto result  = rings.at(index).tryWrite(numberedInput(message));
                                  if (result.admission() == Admission::Written) {
                                      ++sequence;
                                  } else if (result.refusal()->reason == RefusalReason::RingFull) {
                                      ++fullRefusals.at(index);
                                      std::this_thread::yield();
                                  } else {
                                      unexpectedRefusal.at(index) = true;
                                      break;
                                  }
                              }
                              accepted.at(index) = sequence;
                          });
                      }

                      std::thread consumer([&] {
                          start.count_down();
                          start.wait();
                          initialFull.wait();
                          beginDrain.count_down();
                          std::size_t total = 0;
                          while (total < ringCount * recordsPerProducer && std::chrono::steady_clock::now() < deadline) {
                              bool progressed = false;
                              for (std::size_t index = 0; index < ringCount; ++index) {
                                  const auto view = rings.at(index).drain();
                                  for (const auto& record : view.first())
                                      received.at(index).emplace_back(record.message());
                                  for (const auto& record : view.second())
                                      received.at(index).emplace_back(record.message());
                                  if (!view.empty()) {
                                      acknowledgementFailed |= !rings.at(index).acknowledge(view, view.size());
                                      total                 += view.size();
                                      progressed             = true;
                                  }
                              }
                              if (!progressed)
                                  std::this_thread::yield();
                          }
                      });

                      std::thread monitor([&] {
                          start.count_down();
                          start.wait();
                          monitorReady.count_down();
                          std::array<std::uint64_t, ringCount> prior{};
                          while (!stopMonitor.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                              for (std::size_t index = 0; index < ringCount; ++index) {
                                  const auto current = rings.at(index).refusalCount();
                                  if (current < prior.at(index))
                                      counterRegressed = true;
                                  prior.at(index) = current;
                              }
                              std::this_thread::yield();
                          }
                      });

                      for (auto& producer : producers)
                          producer.join();
                      consumer.join();
                      stopMonitor.store(true, std::memory_order_release);
                      monitor.join();

                      checks.expect(!acknowledgementFailed, "every aggregate drain is acknowledged");
                      checks.expect(!counterRegressed, "concurrent observer sees nondecreasing atomic counts");
                      for (std::size_t index = 0; index < ringCount; ++index) {
                          checks.expect(!unexpectedRefusal.at(index), "valid input is refused only for saturation");
                          checks.expect(accepted.at(index) == recordsPerProducer, "producer finishes within the bounded deadline");
                          checks.expect(received.at(index).size() == recordsPerProducer, "consumer receives every admitted value exactly once");
                          for (std::size_t sequence = 0; sequence < received.at(index).size(); ++sequence)
                              checks.expect(received.at(index).at(sequence) == std::to_string(sequence), "per-ring values retain their sequence order");
                          checks.expect(rings.at(index).refusalCount() == fullRefusals.at(index), "atomic counter equals this ring's RingFull results");
                          checks.expect(rings.at(index).writeSequence() == rings.at(index).readSequence(), "ring is fully drained");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
