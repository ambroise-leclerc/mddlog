/**
 * @brief Sync/async delivery: expected records are delivered exactly once, and per-producer
 *        ordering is preserved under concurrent producers.
 */
import std;
import speclab;
import mddlog;

#include "../framework/RecordingSink.hpp"

namespace {

using mddlog::SimpleLogger;
using mddlog::spec::RecordingSink;

const speclab::Register syncDeliveryIsImmediate{"A synchronous logger delivers a record before log() returns", "unit", [] {
                                                    struct State {
                                                        SimpleLogger                   logger{"delivery-sync", false};
                                                        std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
                                                    };
                                                    return speclab::Test<State>("delivery-sync-immediate")
                                                        .Given("a sink attached to a synchronous logger",
                                                               [](State& s) {
                                                                   s.logger.addSink(s.sink);
                                                               })
                                                        .When("a message is logged",
                                                              [](State& s) {
                                                                  s.logger.info("synchronous");
                                                              })
                                                        .Then("it is already present in the sink, with no flush needed",
                                                              [](State& s) {
                                                                  if (s.sink->size() != 1) {
                                                                      throw speclab::core::AssertionFailure(
                                                                          std::format("expected 1 record immediately after log(), got {}", s.sink->size()),
                                                                          std::source_location::current());
                                                                  }
                                                              })
                                                        .Execute();
                                                }};

constexpr int kAsyncOnceCount = 200;

const speclab::Register asyncDeliversExactlyOnce{"An asynchronous logger delivers every accepted record exactly once", "unit", [] {
                                                     struct State {
                                                         SimpleLogger                   logger{"delivery-async", true};
                                                         std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
                                                     };
                                                     return speclab::Test<State>("delivery-async-exactly-once")
                                                         .Given("a sink attached to an asynchronous logger",
                                                                [](State& s) {
                                                                    s.logger.addSink(s.sink);
                                                                })
                                                         .When("200 messages are logged and a flush barrier is awaited",
                                                               [](State& s) {
                                                                   for (int i = 0; i < kAsyncOnceCount; ++i) {
                                                                       s.logger.info(std::format("message-{}", i));
                                                                   }
                                                                   s.logger.flush();
                                                               })
                                                         .Then("exactly 200 distinct records were delivered, none lost or duplicated",
                                                               [](State& s) {
                                                                   speclab::core::Checks checks;
                                                                   const auto            records = s.sink->records();
                                                                   checks.expect(records.size() == static_cast<std::size_t>(kAsyncOnceCount),
                                                                                 std::format("expected {} records, got {}", kAsyncOnceCount, records.size()));
                                                                   std::set<std::string> seen;
                                                                   for (const auto& record : records) {
                                                                       seen.insert(record.message);
                                                                   }
                                                                   checks.expect(seen.size() == records.size(),
                                                                                 "every delivered message is unique (no duplicates)");
                                                                   checks.raise();
                                                               })
                                                         .Execute();
                                                 }};

constexpr int kOrderingProducers   = 4;
constexpr int kOrderingPerProducer = 50;

const speclab::Register perProducerOrderingPreserved{
    "Per-producer ordering is preserved under concurrent async producers",
    "unit",
    [] {
        struct State {
            SimpleLogger                   logger{"delivery-ordering", true};
            std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
        };
        return speclab::Test<State>("delivery-per-producer-ordering")
            .Given("a sink attached to an asynchronous logger",
                   [](State& s) {
                       s.logger.addSink(s.sink);
                   })
            .When("several threads each log a numbered sequence concurrently",
                  [](State& s) {
                      std::vector<std::thread> producers;
                      producers.reserve(kOrderingProducers);
                      for (int p = 0; p < kOrderingProducers; ++p) {
                          producers.emplace_back([&s, p] {
                              for (int i = 0; i < kOrderingPerProducer; ++i) {
                                  s.logger.info(std::format("p{}-{}", p, i), "ordering");
                              }
                          });
                      }
                      for (auto& t : producers) {
                          t.join();
                      }
                      s.logger.flush();
                  })
            .Then("within each producer's messages, the sequence numbers are strictly increasing",
                  [](State& s) {
                      speclab::core::Checks checks;
                      const auto            records = s.sink->records();
                      checks.expect(records.size() == static_cast<std::size_t>(kOrderingProducers) * static_cast<std::size_t>(kOrderingPerProducer),
                                    std::format("expected {} records, got {}", kOrderingProducers * kOrderingPerProducer, records.size()));

                      std::vector<int> lastSeen(kOrderingProducers, -1);
                      bool             orderedWithinProducer = true;
                      for (const auto& record : records) {
                          // Messages are exactly "p<producer>-<index>"; parse strictly rather than with sscanf.
                          const std::string_view text{record.message};
                          int                    producer = -1;
                          int                    index    = -1;
                          bool                   parsed   = false;
                          if (const auto dash = text.find('-'); text.starts_with('p') && dash != std::string_view::npos) {
                              const std::string_view producerDigits = text.substr(1, dash - 1);
                              const std::string_view indexDigits    = text.substr(dash + 1);
                              const auto [producerEnd, producerEc]  = std::from_chars(producerDigits.begin(), producerDigits.end(), producer);
                              const auto [indexEnd, indexEc]        = std::from_chars(indexDigits.begin(), indexDigits.end(), index);
                              parsed                                = producerEc == std::errc{} && producerEnd == producerDigits.end() && indexEc == std::errc{}
                                       && indexEnd == indexDigits.end();
                          }
                          if (parsed && producer >= 0 && producer < kOrderingProducers) {
                              if (index <= lastSeen[static_cast<std::size_t>(producer)]) {
                                  orderedWithinProducer = false;
                              }
                              lastSeen[static_cast<std::size_t>(producer)] = index;
                          }
                      }
                      checks.expect(orderedWithinProducer, "every producer's messages arrived in the order it sent them");
                      for (int p = 0; p < kOrderingProducers; ++p) {
                          checks.expect(lastSeen[static_cast<std::size_t>(p)] == kOrderingPerProducer - 1,
                                        std::format("producer {} delivered its full sequence", p));
                      }
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
