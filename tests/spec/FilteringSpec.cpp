/**
 * @brief Filtering and routing: logger/sink thresholds, enablement, multiple sinks, removal,
 *        clearing.
 *
 * All scenarios use a synchronous logger (asyncLogging=false), so a record is visible in a sink
 * immediately after the log*() call returns and no flush()/synchronization is needed here -
 * asynchronous delivery has its own coverage in DeliverySpec.cpp and FlushSpec.cpp.
 */
import std;
import speclab;
import mddlog;

#include "../framework/RecordingSink.hpp"

namespace {

using mddlog::LogLevel;
using mddlog::SimpleLogger;
using mddlog::spec::RecordingSink;

struct SingleSinkState {
    SimpleLogger                   logger{"filter-test", false};
    std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
};

const speclab::Register loggerMinLevelFilters{"The logger's minimum level filters records below the threshold", "unit", [] {
                                                  return speclab::Test<SingleSinkState>("filtering-logger-min-level")
                                                      .Given("a logger with minimum level WARN and a sink that accepts everything",
                                                             [](SingleSinkState& s) {
                                                                 s.logger.addSink(s.sink);
                                                                 s.logger.setMinLevel(LogLevel::WARN);
                                                             })
                                                      .When("an INFO and a WARN message are logged",
                                                            [](SingleSinkState& s) {
                                                                s.logger.info("below threshold");
                                                                s.logger.warn("at threshold");
                                                            })
                                                      .Then("only the WARN record was delivered",
                                                            [](SingleSinkState& s) {
                                                                speclab::core::Checks checks;
                                                                const auto            records = s.sink->records();
                                                                checks.expect(records.size() == 1, std::format("expected 1 record, got {}", records.size()));
                                                                if (!records.empty()) {
                                                                    checks.expect(records.front().level == LogLevel::WARN,
                                                                                  "the delivered record is the WARN one");
                                                                    checks.expect(records.front().message == "at threshold",
                                                                                  "the delivered record carries the WARN message");
                                                                }
                                                                checks.raise();
                                                            })
                                                      .Execute();
                                              }};

const speclab::Register sinkMinLevelFiltersIndependently{"A sink's minimum level filters independently of the logger's", "unit", [] {
                                                             return speclab::Test<SingleSinkState>("filtering-sink-min-level")
                                                                 .Given("a logger with minimum level TRACE and a sink accepting only ERROR and above",
                                                                        [](SingleSinkState& s) {
                                                                            s.sink->setMinLevel(LogLevel::ERROR);
                                                                            s.logger.addSink(s.sink);
                                                                            s.logger.setMinLevel(LogLevel::TRACE);
                                                                        })
                                                                 .When("an INFO and an ERROR message are logged",
                                                                       [](SingleSinkState& s) {
                                                                           s.logger.info("passes the logger, not the sink");
                                                                           s.logger.error("passes both");
                                                                       })
                                                                 .Then("only the ERROR record reached the sink",
                                                                       [](SingleSinkState& s) {
                                                                           speclab::core::Checks checks;
                                                                           const auto            records = s.sink->records();
                                                                           checks.expect(records.size() == 1,
                                                                                         std::format("expected 1 record, got {}", records.size()));
                                                                           if (!records.empty()) {
                                                                               checks.expect(records.front().level == LogLevel::ERROR,
                                                                                             "the delivered record is the ERROR one");
                                                                           }
                                                                           checks.raise();
                                                                       })
                                                                 .Execute();
                                                         }};

const speclab::Register disablingLoggerDropsRecords{"Disabling the logger drops ordinary records", "unit", [] {
                                                        return speclab::Test<SingleSinkState>("filtering-logger-disabled")
                                                            .Given("a disabled logger with a sink attached",
                                                                   [](SingleSinkState& s) {
                                                                       s.logger.addSink(s.sink);
                                                                       s.logger.setEnabled(false);
                                                                   })
                                                            .When("an INFO message is logged",
                                                                  [](SingleSinkState& s) {
                                                                      s.logger.info("dropped");
                                                                  })
                                                            .Then("nothing reached the sink",
                                                                  [](SingleSinkState& s) {
                                                                      if (s.sink->size() != 0) {
                                                                          throw speclab::core::AssertionFailure(
                                                                              std::format("expected 0 records while disabled, got {}", s.sink->size()),
                                                                              std::source_location::current());
                                                                      }
                                                                  })
                                                            .Execute();
                                                    }};

const speclab::Register reEnablingLoggerResumesDelivery{"Re-enabling the logger resumes delivery", "unit", [] {
                                                            return speclab::Test<SingleSinkState>("filtering-logger-re-enabled")
                                                                .Given("a logger disabled and then re-enabled",
                                                                       [](SingleSinkState& s) {
                                                                           s.logger.addSink(s.sink);
                                                                           s.logger.setEnabled(false);
                                                                           s.logger.info("dropped while disabled");
                                                                           s.logger.setEnabled(true);
                                                                       })
                                                                .When("an INFO message is logged after re-enabling",
                                                                      [](SingleSinkState& s) {
                                                                          s.logger.info("delivered after re-enable");
                                                                      })
                                                                .Then("exactly the post-re-enable record was delivered",
                                                                      [](SingleSinkState& s) {
                                                                          speclab::core::Checks checks;
                                                                          const auto            records = s.sink->records();
                                                                          checks.expect(records.size() == 1,
                                                                                        std::format("expected 1 record, got {}", records.size()));
                                                                          if (!records.empty()) {
                                                                              checks.expect(records.front().message == "delivered after re-enable",
                                                                                            "the surviving record is the post-re-enable one");
                                                                          }
                                                                          checks.raise();
                                                                      })
                                                                .Execute();
                                                        }};

struct TwoSinkState {
    SimpleLogger                   logger{"filter-test-2", false};
    std::shared_ptr<RecordingSink> sinkA = std::make_shared<RecordingSink>("A");
    std::shared_ptr<RecordingSink> sinkB = std::make_shared<RecordingSink>("B");
};

const speclab::Register multipleSinksAllReceive{"Every attached sink receives a logged record", "unit", [] {
                                                    return speclab::Test<TwoSinkState>("filtering-multiple-sinks")
                                                        .Given("two sinks attached to the same logger",
                                                               [](TwoSinkState& s) {
                                                                   s.logger.addSink(s.sinkA);
                                                                   s.logger.addSink(s.sinkB);
                                                               })
                                                        .When("one message is logged",
                                                              [](TwoSinkState& s) {
                                                                  s.logger.info("broadcast");
                                                              })
                                                        .Then("both sinks recorded exactly one message",
                                                              [](TwoSinkState& s) {
                                                                  speclab::core::Checks checks;
                                                                  checks.expect(s.sinkA->size() == 1, "sink A received the record");
                                                                  checks.expect(s.sinkB->size() == 1, "sink B received the record");
                                                                  checks.expect(s.logger.getSinkCount() == 2, "the logger reports two sinks");
                                                                  checks.raise();
                                                              })
                                                        .Execute();
                                                }};

const speclab::Register removeSinkRemovesOnlyNamed{"removeSink() removes only the named sink", "unit", [] {
                                                       return speclab::Test<TwoSinkState>("filtering-remove-sink")
                                                           .Given("two sinks attached, one of which will be removed",
                                                                  [](TwoSinkState& s) {
                                                                      s.logger.addSink(s.sinkA);
                                                                      s.logger.addSink(s.sinkB);
                                                                      s.logger.removeSink("A");
                                                                  })
                                                           .When("one message is logged",
                                                                 [](TwoSinkState& s) {
                                                                     s.logger.info("after removal");
                                                                 })
                                                           .Then("the removed sink stayed silent and the remaining sink still recorded it",
                                                                 [](TwoSinkState& s) {
                                                                     speclab::core::Checks checks;
                                                                     checks.expect(s.sinkA->size() == 0, "the removed sink A received nothing");
                                                                     checks.expect(s.sinkB->size() == 1, "sink B still received the record");
                                                                     checks.expect(s.logger.getSinkCount() == 1, "one sink remains attached");
                                                                     checks.raise();
                                                                 })
                                                           .Execute();
                                                   }};

const speclab::Register clearSinksRemovesEverySink{"clearSinks() removes every sink and further records are simply dropped", "unit", [] {
                                                       return speclab::Test<TwoSinkState>("filtering-clear-sinks")
                                                           .Given("two sinks attached and then cleared",
                                                                  [](TwoSinkState& s) {
                                                                      s.logger.addSink(s.sinkA);
                                                                      s.logger.addSink(s.sinkB);
                                                                      s.logger.clearSinks();
                                                                  })
                                                           .When("one message is logged",
                                                                 [](TwoSinkState& s) {
                                                                     s.logger.info("into the void");
                                                                 })
                                                           .Then("no sink recorded anything and the sink count is zero",
                                                                 [](TwoSinkState& s) {
                                                                     speclab::core::Checks checks;
                                                                     checks.expect(s.sinkA->size() == 0, "sink A received nothing");
                                                                     checks.expect(s.sinkB->size() == 0, "sink B received nothing");
                                                                     checks.expect(s.logger.getSinkCount() == 0, "no sinks remain attached");
                                                                     checks.raise();
                                                                 })
                                                           .Execute();
                                                   }};

}  // namespace
