/**
 * @brief Concurrency: ConsoleSink's option getters/setters have defined synchronization, and
 *        concurrent producers logging through a shared logger neither lose nor duplicate records.
 */
import std;
import speclab;
import mddlog;

#include "../framework/RecordingSink.hpp"

namespace {

using mddlog::ConsoleSink;
using mddlog::SimpleLogger;
using mddlog::spec::RecordingSink;

const speclab::Register consoleSinkOptionsSurviveConcurrentAccess{"ConsoleSink's color/stderr options survive concurrent getter and setter access", "unit", [] {
                                                                      struct State {
                                                                          ConsoleSink sink{false, false};
                                                                      };
                                                                      return speclab::Test<State>("concurrency-console-sink-options")
                                                                          .Given("a console sink shared by several threads", [](State&) {})
                                                                          .When("threads concurrently flip and read both options many times",
                                                                                [](State& s) {
                                                                                    constexpr int            kIterations = 2000;
                                                                                    std::vector<std::thread> threads;
                                                                                    threads.emplace_back([&s] {
                                                                                        for (int i = 0; i < kIterations; ++i) {
                                                                                            s.sink.setColorEnabled(i % 2 == 0);
                                                                                        }
                                                                                    });
                                                                                    threads.emplace_back([&s] {
                                                                                        for (int i = 0; i < kIterations; ++i) {
                                                                                            s.sink.setStderrEnabled(i % 2 == 0);
                                                                                        }
                                                                                    });
                                                                                    threads.emplace_back([&s] {
                                                                                        for (int i = 0; i < kIterations; ++i) {
                                                                                            [[maybe_unused]] volatile bool v = s.sink.isColorEnabled();
                                                                                        }
                                                                                    });
                                                                                    threads.emplace_back([&s] {
                                                                                        for (int i = 0; i < kIterations; ++i) {
                                                                                            [[maybe_unused]] volatile bool v = s.sink.isStderrEnabled();
                                                                                        }
                                                                                    });
                                                                                    for (auto& t : threads) {
                                                                                        t.join();
                                                                                    }
                                                                                })
                                                                          .Then("no crash or corruption occurred and the sink still reports a valid state",
                                                                                [](State& s) {
                                                                                    // The assertion is that the mutex-guarded getters/setters above completed
                                                                                    // without a crash (this repo's sanitizer CI job runs ASan+UBSan, which
                                                                                    // catch memory errors and undefined behavior but not data races on their
                                                                                    // own - ThreadSanitizer would be the tool for that and is not part of this
                                                                                    // suite); reading the final state once more here documents that the object
                                                                                    // is still in a well-defined state afterward.
                                                                                    [[maybe_unused]] bool color     = s.sink.isColorEnabled();
                                                                                    [[maybe_unused]] bool useStderr = s.sink.isStderrEnabled();
                                                                                })
                                                                          .Execute();
                                                                  }};

constexpr int kConcurrencyProducers   = 8;
constexpr int kConcurrencyPerProducer = 100;

const speclab::Register concurrentProducersNeitherLoseNorDuplicate{
    "Concurrent producers logging through a shared logger neither lose nor duplicate records",
    "unit",
    [] {
        struct State {
            SimpleLogger                   logger{"concurrency-test", true};
            std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
        };
        return speclab::Test<State>("concurrency-no-loss-no-duplication")
            .Given("a sink attached to an asynchronous logger",
                   [](State& s) {
                       s.logger.addSink(s.sink);
                   })
            .When("8 threads each log 100 uniquely-identified messages concurrently",
                  [](State& s) {
                      std::vector<std::thread> producers;
                      producers.reserve(kConcurrencyProducers);
                      for (int p = 0; p < kConcurrencyProducers; ++p) {
                          producers.emplace_back([&s, p] {
                              for (int i = 0; i < kConcurrencyPerProducer; ++i) {
                                  s.logger.info(std::format("id-{}-{}", p, i), "concurrency");
                              }
                          });
                      }
                      for (auto& t : producers) {
                          t.join();
                      }
                      s.logger.flush();
                  })
            .Then("every one of the 800 unique ids was delivered exactly once",
                  [](State& s) {
                      speclab::core::Checks checks;
                      const auto            records  = s.sink->records();
                      constexpr std::size_t expected = static_cast<std::size_t>(kConcurrencyProducers) * kConcurrencyPerProducer;
                      checks.expect(records.size() == expected, std::format("expected {} records, got {}", expected, records.size()));

                      std::map<std::string, int> occurrences;
                      for (const auto& record : records) {
                          ++occurrences[record.message];
                      }
                      bool allExactlyOnce = true;
                      for (int p = 0; p < kConcurrencyProducers; ++p) {
                          for (int i = 0; i < kConcurrencyPerProducer; ++i) {
                              const auto key = std::format("id-{}-{}", p, i);
                              const auto it  = occurrences.find(key);
                              if (it == occurrences.end() || it->second != 1) {
                                  allExactlyOnce = false;
                              }
                          }
                      }
                      checks.expect(allExactlyOnce, "every expected id appears in the delivered records exactly once");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
