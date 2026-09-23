/**
 * @brief Sink failures: a throwing sink does not prevent delivery to other sinks, does not cause
 *        an unhandled worker-thread exception, and its failure is reported explicitly rather than
 *        silently swallowed.
 */
import std;
import speclab;
import mddlog;

#include "../framework/RecordingSink.hpp"

namespace {

using mddlog::SimpleLogger;
using mddlog::spec::RecordingSink;
using mddlog::spec::ThrowingSink;

struct SyncState {
    SimpleLogger                   logger{"sink-failure-sync", false};
    std::shared_ptr<ThrowingSink>  throwing  = std::make_shared<ThrowingSink>();
    std::shared_ptr<RecordingSink> recording = std::make_shared<RecordingSink>();
};

const speclab::Register throwingSinkDoesNotBlockOthers{"A throwing sink does not prevent delivery to the other sinks", "unit", [] {
                                                           return speclab::Test<SyncState>("sink-failure-does-not-block-others")
                                                               .Given("a throwing sink and a recording sink both attached, synchronously",
                                                                      [](SyncState& s) {
                                                                          s.logger.addSink(s.throwing);
                                                                          s.logger.addSink(s.recording);
                                                                      })
                                                               .When("a message is logged",
                                                                     [](SyncState& s) {
                                                                         s.logger.info("survives the throw");
                                                                     })
                                                               .Then("the recording sink still received it, and the call did not propagate the throw",
                                                                     [](SyncState& s) {
                                                                         speclab::core::Checks checks;
                                                                         checks.expect(s.throwing->attempts() == 1, "the throwing sink was invoked once");
                                                                         checks.expect(s.recording->size() == 1,
                                                                                       "the recording sink still received the record");
                                                                         checks.raise();
                                                                     })
                                                               .Execute();
                                                       }};

const speclab::Register writeFailureIsRecordedExplicitly{"A caught sink write failure is recorded explicitly in that sink's own statistics", "unit", [] {
                                                             return speclab::Test<SyncState>("sink-failure-recorded-explicitly")
                                                                 .Given("a throwing sink attached to a logger",
                                                                        [](SyncState& s) {
                                                                            s.logger.addSink(s.throwing);
                                                                        })
                                                                 .When("two messages are logged",
                                                                       [](SyncState& s) {
                                                                           s.logger.info("first");
                                                                           s.logger.info("second");
                                                                       })
                                                                 .Then("the throwing sink's dropped-record count reflects both failures",
                                                                       [](SyncState& s) {
                                                                           const auto& stats = s.throwing->getStatistics();
                                                                           if (stats.recordsDropped.load() != 2) {
                                                                               throw speclab::core::AssertionFailure(
                                                                                   std::format("expected recordsDropped == 2, got {}",
                                                                                               stats.recordsDropped.load()),
                                                                                   std::source_location::current());
                                                                           }
                                                                       })
                                                                 .Execute();
                                                         }};

struct AsyncState {
    SimpleLogger                   logger{"sink-failure-async", true};
    std::shared_ptr<ThrowingSink>  throwing  = std::make_shared<ThrowingSink>();
    std::shared_ptr<RecordingSink> recording = std::make_shared<RecordingSink>();
};

const speclab::Register throwingSinkDoesNotKillAsyncWorker{
    "A throwing sink does not crash the asynchronous worker thread",
    "unit",
    [] {
        return speclab::Test<AsyncState>("sink-failure-async-worker-survives")
            .Given("a throwing sink and a recording sink attached to an asynchronous logger",
                   [](AsyncState& s) {
                       s.logger.addSink(s.throwing);
                       s.logger.addSink(s.recording);
                   })
            .When("several messages are logged and flushed, so the worker must process each one",
                  [](AsyncState& s) {
                      for (int i = 0; i < 20; ++i) {
                          s.logger.info(std::format("msg-{}", i));
                      }
                      s.logger.flush();
                  })
            .Then("the worker kept running: every message still reached the recording sink",
                  [](AsyncState& s) {
                      speclab::core::Checks checks;
                      checks.expect(s.throwing->attempts() == 20, std::format("throwing sink saw 20 attempts, got {}", s.throwing->attempts()));
                      checks.expect(s.recording->size() == 20, std::format("recording sink saw 20 deliveries, got {}", s.recording->size()));
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
