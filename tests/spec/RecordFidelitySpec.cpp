/**
 * @brief Record fidelity: severity, message, category, medical/audit fields, producer thread
 *        identity, and caller source location.
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
    SimpleLogger                   logger{"fidelity-test", false};
    std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
};

const speclab::Register basicFieldsPreserved{"A basic log() call preserves level, message, and category", "unit", [] {
                                                 return speclab::Test<SingleSinkState>("fidelity-basic-fields")
                                                     .Given("a sink attached to a logger",
                                                            [](SingleSinkState& s) {
                                                                s.logger.addSink(s.sink);
                                                            })
                                                     .When("a WARN message is logged with an explicit category",
                                                           [](SingleSinkState& s) {
                                                               s.logger.warn("elevated temperature", "vitals");
                                                           })
                                                     .Then("the record carries that exact level, message and category",
                                                           [](SingleSinkState& s) {
                                                               speclab::core::Checks checks;
                                                               const auto            records = s.sink->records();
                                                               checks.expect(records.size() == 1, "one record was delivered");
                                                               if (!records.empty()) {
                                                                   const auto& r = records.front();
                                                                   checks.expect(r.level == LogLevel::Warn, "level is WARN");
                                                                   checks.expect(r.message == "elevated temperature", "message matches");
                                                                   checks.expect(r.category == "vitals", "category matches");
                                                               }
                                                               checks.raise();
                                                           })
                                                     .Execute();
                                             }};

const speclab::Register medicalFieldsPreserved{
    "logMedical() preserves user, session and device identifiers",
    "unit",
    [] {
        return speclab::Test<SingleSinkState>("fidelity-medical-fields")
            .Given("a sink attached to a logger",
                   [](SingleSinkState& s) {
                       s.logger.addSink(s.sink);
                   })
            .When("a medical compliance record is logged",
                  [](SingleSinkState& s) {
                      s.logger.logMedical(LogLevel::Info, "monitoring started", "patient_monitor", "user123", "session456", "device789");
                  })
            .Then("every medical field round-trips",
                  [](SingleSinkState& s) {
                      speclab::core::Checks checks;
                      const auto            records = s.sink->records();
                      checks.expect(records.size() == 1, "one record was delivered");
                      if (!records.empty()) {
                          const auto& r = records.front();
                          checks.expect(r.category == "patient_monitor", "category matches");
                          checks.expect(r.userId == "user123", "userId matches");
                          checks.expect(r.sessionId == "session456", "sessionId matches");
                          checks.expect(r.deviceId == "device789", "deviceId matches");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register auditFieldsPreserved{"logAudit() preserves the event type, risk level and AUDIT severity", "unit", [] {
                                                 return speclab::Test<SingleSinkState>("fidelity-audit-fields")
                                                     .Given("a sink attached to a logger",
                                                            [](SingleSinkState& s) {
                                                                s.logger.addSink(s.sink);
                                                            })
                                                     .When("an audit event is logged",
                                                           [](SingleSinkState& s) {
                                                               s.logger.logAudit("data accessed", "DATA_ACCESS", "user123", "device789", "LOW");
                                                           })
                                                     .Then("the record is AUDIT severity with the event type and risk level intact",
                                                           [](SingleSinkState& s) {
                                                               speclab::core::Checks checks;
                                                               const auto            records = s.sink->records();
                                                               checks.expect(records.size() == 1, "one record was delivered");
                                                               if (!records.empty()) {
                                                                   const auto& r = records.front();
                                                                   checks.expect(r.level == LogLevel::Audit, "level is AUDIT");
                                                                   checks.expect(r.category == "audit", "category is 'audit'");
                                                                   checks.expect(r.auditEventType == "DATA_ACCESS", "event type matches");
                                                                   checks.expect(r.riskLevel == "LOW", "risk level matches");
                                                                   checks.expect(r.userId == "user123", "userId matches");
                                                                   checks.expect(r.deviceId == "device789", "deviceId matches");
                                                               }
                                                               checks.raise();
                                                           })
                                                     .Execute();
                                             }};

const speclab::Register threadIdentityPreserved{"The record's thread id identifies the producer that logged it", "unit", [] {
                                                    return speclab::Test<SingleSinkState>("fidelity-thread-identity")
                                                        .Given("a sink attached to a logger",
                                                               [](SingleSinkState& s) {
                                                                   s.logger.addSink(s.sink);
                                                               })
                                                        .When("a message is logged from this thread",
                                                              [](SingleSinkState& s) {
                                                                  s.logger.info("who logged this?");
                                                              })
                                                        .Then("the record's thread id equals the calling thread's id",
                                                              [](SingleSinkState& s) {
                                                                  const auto records = s.sink->records();
                                                                  if (records.empty() || records.front().threadId != std::this_thread::get_id()) {
                                                                      throw speclab::core::AssertionFailure("record thread id did not match the logging thread",
                                                                                                            std::source_location::current());
                                                                  }
                                                              })
                                                        .Execute();
                                                }};

// Correctness regression for the internal-location capture bug: LogRecord's constructor defaults
// std::source_location::current() at ITS OWN call site, so every Logger convenience method
// (log/info/warn/...) must accept and forward an explicit source_location parameter, or every
// record would report a line inside Logger.cppm instead of the application's actual call site.
const speclab::Register sourceLocationPointsAtCallSite{
    "The captured source location points at the caller, not inside Logger.cppm",
    "unit",
    [] {
        struct State {
            SimpleLogger                   logger{"fidelity-location-test", false};
            std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
            std::size_t                    expectedLine{0};
        };
        return speclab::Test<State>("fidelity-source-location")
            .Given("a sink attached to a logger",
                   [](State& s) {
                       s.logger.addSink(s.sink);
                   })
            .When("a message is logged from a known line in this file",
                  [](State& s) {
                      s.expectedLine = std::source_location::current().line() + 1;
                      s.logger.info("locate me");
                  })
            .Then("the record's location names this file and that exact line",
                  [](State& s) {
                      speclab::core::Checks checks;
                      const auto            records = s.sink->records();
                      checks.expect(records.size() == 1, "one record was delivered");
                      if (!records.empty()) {
                          const std::string_view file = records.front().location.file_name();
                          checks.expect(file.find("RecordFidelitySpec.cpp") != std::string_view::npos,
                                        std::format("location file is this spec file, got '{}'", file));
                          checks.expect(file.find("Logger.cppm") == std::string_view::npos, "location file is not inside Logger.cppm");
                          checks.expect(records.front().location.line() == s.expectedLine,
                                        std::format("location line is {}, expected {}", records.front().location.line(), s.expectedLine));
                      }
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
