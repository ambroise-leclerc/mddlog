/**
 * @brief Flush and destruction: a flush barrier completes only after previously accepted records
 *        are delivered, and destroying the logger drains records already accepted into the queue.
 */
import std;
import speclab;
import mddlog;

#include "../framework/RecordingSink.hpp"

namespace {

using mddlog::SimpleLogger;
using mddlog::spec::RecordingSink;

const speclab::Register flushWaitsForAcceptedRecords{
    "flush() returns only after every previously accepted record was delivered", "unit", [] {
        struct State {
            std::unique_ptr<SimpleLogger> logger;
            std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
        };
        return speclab::Test<State>("flush-waits-for-accepted-records")
            .Given("an asynchronous logger with a sink attached",
                   [](State& s) {
                       s.logger = std::make_unique<SimpleLogger>("flush-test", true);
                       s.logger->addSink(s.sink);
                   })
            .When("100 records are logged and flush() is called",
                  [](State& s) {
                      for (int i = 0; i < 100; ++i) {
                          s.logger->info(std::format("flush-{}", i));
                      }
                      s.logger->flush();
                  })
            .Then("all 100 records are already in the sink the instant flush() returns",
                  [](State& s) {
                      if (s.sink->size() != 100) {
                          throw speclab::core::AssertionFailure(
                              std::format("expected 100 records after flush(), got {}", s.sink->size()),
                              std::source_location::current());
                      }
                  })
            .Execute();
    }};

const speclab::Register flushIsABarrierNotADrainSkip{
    "A second flush() after more records still waits for those too", "unit", [] {
        struct State {
            SimpleLogger logger{"flush-barrier-test", true};
            std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
        };
        return speclab::Test<State>("flush-is-a-repeatable-barrier")
            .Given("a sink attached to an asynchronous logger",
                   [](State& s) { s.logger.addSink(s.sink); })
            .When("records are logged and flushed twice, in two separate batches",
                  [](State& s) {
                      for (int i = 0; i < 10; ++i) {
                          s.logger.info(std::format("batch1-{}", i));
                      }
                      s.logger.flush();
                      for (int i = 0; i < 10; ++i) {
                          s.logger.info(std::format("batch2-{}", i));
                      }
                      s.logger.flush();
                  })
            .Then("both batches are fully delivered",
                  [](State& s) {
                      if (s.sink->size() != 20) {
                          throw speclab::core::AssertionFailure(
                              std::format("expected 20 records after two flushes, got {}", s.sink->size()),
                              std::source_location::current());
                      }
                  })
            .Execute();
    }};

const speclab::Register destructionDrainsAcceptedRecords{
    "Destroying the logger drains records already accepted into its queue", "unit", [] {
        struct State {
            std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
        };
        return speclab::Test<State>("flush-destruction-drains")
            .Given("a sink that will outlive the logger", [](State&) {})
            .When("an asynchronous logger logs records and is destroyed without an explicit flush()",
                  [](State& s) {
                      {
                          SimpleLogger logger{"drain-on-destroy", true};
                          logger.addSink(s.sink);
                          for (int i = 0; i < 50; ++i) {
                              logger.info(std::format("drain-{}", i));
                          }
                          // No flush() call: the destructor itself must drain everything already
                          // accepted before the async worker thread is joined.
                      }
                  })
            .Then("all 50 records were delivered before the logger's destructor returned",
                  [](State& s) {
                      if (s.sink->size() != 50) {
                          throw speclab::core::AssertionFailure(
                              std::format("expected 50 records drained by destruction, got {}",
                                          s.sink->size()),
                              std::source_location::current());
                      }
                  })
            .Execute();
    }};

}  // namespace
