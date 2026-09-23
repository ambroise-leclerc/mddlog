/**
 * @brief Audit policy: logAudit() intentionally bypasses the logger's own disablement and minimum
 *        level filtering, so a compliance audit trail cannot be silenced by reconfiguring the
 *        logger. It still goes through each sink's own enabled/disabled state, since that is a
 *        property of the sink's delivery channel rather than of the logger's filtering policy.
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
    SimpleLogger                   logger{"audit-policy-test", false};
    std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
};

const speclab::Register auditBypassesDisabledLogger{"logAudit() is delivered even while the logger is disabled", "unit", [] {
                                                        return speclab::Test<SingleSinkState>("audit-bypasses-disabled-logger")
                                                            .Given("a disabled logger with a sink attached",
                                                                   [](SingleSinkState& s) {
                                                                       s.logger.addSink(s.sink);
                                                                       s.logger.setEnabled(false);
                                                                   })
                                                            .When("an ordinary message and an audit event are both logged",
                                                                  [](SingleSinkState& s) {
                                                                      s.logger.info("this must be dropped");
                                                                      s.logger.logAudit("this must survive", "DATA_ACCESS", "user123", "device789");
                                                                  })
                                                            .Then("only the audit record was delivered",
                                                                  [](SingleSinkState& s) {
                                                                      speclab::core::Checks checks;
                                                                      const auto            records = s.sink->records();
                                                                      checks.expect(records.size() == 1,
                                                                                    std::format("expected exactly 1 record, got {}", records.size()));
                                                                      if (!records.empty()) {
                                                                          checks.expect(records.front().level == LogLevel::Audit,
                                                                                        "the surviving record is the AUDIT one");
                                                                          checks.expect(records.front().auditEventType == "DATA_ACCESS",
                                                                                        "its event type is intact");
                                                                      }
                                                                      checks.raise();
                                                                  })
                                                            .Execute();
                                                    }};

const speclab::Register auditBypassesMinimumLevel{"logAudit() is delivered regardless of the logger's minimum level", "unit", [] {
                                                      return speclab::Test<SingleSinkState>("audit-bypasses-min-level")
                                                          .Given("a logger whose minimum level is set above ordinary severities",
                                                                 [](SingleSinkState& s) {
                                                                     s.logger.addSink(s.sink);
                                                                     s.logger.setMinLevel(LogLevel::Fatal);
                                                                 })
                                                          .When("an audit event is logged",
                                                                [](SingleSinkState& s) {
                                                                    s.logger.logAudit("configuration changed", "CONFIG_CHANGE", "admin", "device789");
                                                                })
                                                          .Then("it was delivered",
                                                                [](SingleSinkState& s) {
                                                                    if (s.sink->size() != 1) {
                                                                        throw speclab::core::AssertionFailure(
                                                                            std::format("expected the audit record to be delivered, got {} records",
                                                                                        s.sink->size()),
                                                                            std::source_location::current());
                                                                    }
                                                                })
                                                          .Execute();
                                                  }};

const speclab::Register auditStillRespectsDisabledSink{
    "logAudit() does not bypass an explicitly disabled sink",
    "unit",
    [] {
        return speclab::Test<SingleSinkState>("audit-respects-disabled-sink")
            .Given("a sink that is itself disabled",
                   [](SingleSinkState& s) {
                       s.logger.addSink(s.sink);
                       s.sink->setEnabled(false);
                   })
            .When("an audit event is logged",
                  [](SingleSinkState& s) {
                      s.logger.logAudit("should not reach a disabled sink", "DATA_ACCESS", "user123", "device789");
                  })
            .Then("the disabled sink received nothing: the audit bypass is logger-level, not sink-level",
                  [](SingleSinkState& s) {
                      if (s.sink->size() != 0) {
                          throw speclab::core::AssertionFailure(std::format("expected 0 records on a disabled sink, got {}", s.sink->size()),
                                                                std::source_location::current());
                      }
                  })
            .Execute();
    }};

}  // namespace
