/**
 * @brief Characterization tests for mddlog::LogLevel, pinned ahead of the enumerator rename
 *        (TRACE...AUDIT -> Trace...Audit). These scenarios record the library's current,
 *        observable behavior - numeric values, string conversions, color codes, the compliance
 *        threshold, level-based filtering, and ConsoleSink's rendering - so the rename can be
 *        verified as an identifier-only change: the same scenarios pass before and after, with
 *        only the LogLevel::* spellings in this file changing.
 *
 * Deliberately does not fix anything found along the way (e.g. ConsoleSink's stderr routing
 * condition actually covers AUDIT too, not just "ERROR and FATAL" as its doc comment says): a
 * characterization test records what the code does, not what it should do.
 */
import std;
import speclab;
import mddlog;

#include "../framework/RecordingSink.hpp"

namespace {

using mddlog::ConsoleSink;
using mddlog::fromString;
using mddlog::getColorCode;
using mddlog::getResetColorCode;
using mddlog::isComplianceLevel;
using mddlog::LogLevel;
using mddlog::LogRecord;
using mddlog::SimpleLogger;
using mddlog::spec::RecordingSink;
using mddlog::toString;

constexpr std::array allLevels{LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal, LogLevel::Audit};

/// Redirects std::cout/std::cerr to in-memory buffers for the lifetime of the object, so a
/// ConsoleSink's actual output can be inspected. Restoration is unconditional (destructor), so a
/// failed check (Checks::raise()/a thrown AssertionFailure) can never leave the process' standard
/// streams redirected into a buffer that SpecLab's own reporter would otherwise write into.
struct StdStreamCapture {
    std::ostringstream out;
    std::ostringstream err;
    std::streambuf*    savedOut;
    std::streambuf*    savedErr;

    StdStreamCapture() : savedOut(std::cout.rdbuf(out.rdbuf())), savedErr(std::cerr.rdbuf(err.rdbuf())) {}
    ~StdStreamCapture() {
        std::cout.rdbuf(savedOut);
        std::cerr.rdbuf(savedErr);
    }
    StdStreamCapture(const StdStreamCapture&)            = delete;
    StdStreamCapture& operator=(const StdStreamCapture&) = delete;
    StdStreamCapture(StdStreamCapture&&)                 = delete;
    StdStreamCapture& operator=(StdStreamCapture&&)      = delete;
};

const speclab::Register numericValuesUnderlyingTypeAndOrdering{
    "LogLevel's numeric values, underlying type and relative order are pinned",
    "unit",
    [] {
        return speclab::Test("loglevel-numeric-values-and-order")
            .Then("TRACE..AUDIT are 0..6 via std::to_underlying, the underlying type is std::uint8_t, "
                  "and comparisons follow that numeric order - the basis every min-level filter relies on",
                  [] {
                      speclab::core::Checks checks;
                      checks.expect(std::is_same_v<std::underlying_type_t<LogLevel>, std::uint8_t>, "LogLevel's underlying type is std::uint8_t");
                      checks.expect(std::to_underlying(LogLevel::Trace) == 0, "TRACE == 0");
                      checks.expect(std::to_underlying(LogLevel::Debug) == 1, "DEBUG == 1");
                      checks.expect(std::to_underlying(LogLevel::Info) == 2, "INFO == 2");
                      checks.expect(std::to_underlying(LogLevel::Warn) == 3, "WARN == 3");
                      checks.expect(std::to_underlying(LogLevel::Error) == 4, "ERROR == 4");
                      checks.expect(std::to_underlying(LogLevel::Fatal) == 5, "FATAL == 5");
                      checks.expect(std::to_underlying(LogLevel::Audit) == 6, "AUDIT == 6");
                      checks.expect(LogLevel::Trace < LogLevel::Debug, "TRACE < DEBUG");
                      checks.expect(LogLevel::Debug < LogLevel::Info, "DEBUG < INFO");
                      checks.expect(LogLevel::Info < LogLevel::Warn, "INFO < WARN");
                      checks.expect(LogLevel::Warn < LogLevel::Error, "WARN < ERROR");
                      checks.expect(LogLevel::Error < LogLevel::Fatal, "ERROR < FATAL");
                      checks.expect(LogLevel::Fatal < LogLevel::Audit, "FATAL < AUDIT");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register toStringProducesExactStrings{"toString() produces the exact expected string for every level, and \"UNKNOWN\" outside the enum",
                                                     "unit",
                                                     [] {
                                                         return speclab::Test("loglevel-tostring-exact")
                                                             .Then("toString() returns the matching all-caps literal for each level, and \"UNKNOWN\" for "
                                                                   "an out-of-range value",
                                                                   [] {
                                                                       speclab::core::Checks checks;
                                                                       checks.expect(toString(LogLevel::Trace) == "TRACE", "TRACE -> \"TRACE\"");
                                                                       checks.expect(toString(LogLevel::Debug) == "DEBUG", "DEBUG -> \"DEBUG\"");
                                                                       checks.expect(toString(LogLevel::Info) == "INFO", "INFO -> \"INFO\"");
                                                                       checks.expect(toString(LogLevel::Warn) == "WARN", "WARN -> \"WARN\"");
                                                                       checks.expect(toString(LogLevel::Error) == "ERROR", "ERROR -> \"ERROR\"");
                                                                       checks.expect(toString(LogLevel::Fatal) == "FATAL", "FATAL -> \"FATAL\"");
                                                                       checks.expect(toString(LogLevel::Audit) == "AUDIT", "AUDIT -> \"AUDIT\"");
                                                                       checks.expect(toString(static_cast<LogLevel>(99)  /* NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately out of range - this scenario pins the fallback */) == "UNKNOWN",
                                                                                     "an out-of-range value -> \"UNKNOWN\"");
                                                                       checks.raise();
                                                                   })
                                                             .Execute();
                                                     }};

const speclab::Register fromStringParsesEachLevelAndFallsBackToInfo{
    "fromString() parses each canonical string, and falls back to INFO for an unrecognized one",
    "unit",
    [] {
        return speclab::Test("loglevel-fromstring-and-fallback")
            .Then("each canonical string parses to its level, and an unrecognized (or empty) string "
                  "yields INFO",
                  [] {
                      speclab::core::Checks checks;
                      checks.expect(fromString("TRACE") == LogLevel::Trace, "\"TRACE\" -> TRACE");
                      checks.expect(fromString("DEBUG") == LogLevel::Debug, "\"DEBUG\" -> DEBUG");
                      checks.expect(fromString("INFO") == LogLevel::Info, "\"INFO\" -> INFO");
                      checks.expect(fromString("WARN") == LogLevel::Warn, "\"WARN\" -> WARN");
                      checks.expect(fromString("ERROR") == LogLevel::Error, "\"ERROR\" -> ERROR");
                      checks.expect(fromString("FATAL") == LogLevel::Fatal, "\"FATAL\" -> FATAL");
                      checks.expect(fromString("AUDIT") == LogLevel::Audit, "\"AUDIT\" -> AUDIT");
                      checks.expect(fromString("not-a-level") == LogLevel::Info, "an unrecognized string falls back to INFO");
                      checks.expect(fromString("") == LogLevel::Info, "an empty string falls back to INFO");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register roundTripBetweenToStringAndFromString{
    "toString() and fromString() round-trip for every level",
    "unit",
    [] {
        return speclab::Test("loglevel-tostring-fromstring-roundtrip")
            .Then("fromString(toString(level)) recovers the original level, for every level",
                  [] {
                      speclab::core::Checks checks;
                      for (auto level : allLevels) {
                          checks.expect(fromString(toString(level)) == level, std::format("fromString(toString({})) == {}", toString(level), toString(level)));
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register colorCodesForEachLevel{"getColorCode() returns a distinct ANSI sequence for every level, and getResetColorCode() resets it",
                                               "unit",
                                               [] {
                                                   return speclab::Test("loglevel-color-codes")
                                                       .Then("each level has its documented color, an out-of-range value resets instead of "
                                                             "coloring, and the seven levels map to seven distinct sequences",
                                                             [] {
                                                                 speclab::core::Checks checks;
                                                                 checks.expect(getColorCode(LogLevel::Trace) == "\033[37m", "TRACE is white");
                                                                 checks.expect(getColorCode(LogLevel::Debug) == "\033[36m", "DEBUG is cyan");
                                                                 checks.expect(getColorCode(LogLevel::Info) == "\033[32m", "INFO is green");
                                                                 checks.expect(getColorCode(LogLevel::Warn) == "\033[33m", "WARN is yellow");
                                                                 checks.expect(getColorCode(LogLevel::Error) == "\033[31m", "ERROR is red");
                                                                 checks.expect(getColorCode(LogLevel::Fatal) == "\033[35m", "FATAL is magenta");
                                                                 checks.expect(getColorCode(LogLevel::Audit) == "\033[1;34m", "AUDIT is bold blue");
                                                                 checks.expect(getColorCode(static_cast<LogLevel>(99)  /* NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately out of range - this scenario pins the fallback */) == "\033[0m", "an out-of-range value resets rather than coloring");
                                                                 checks.expect(getResetColorCode() == "\033[0m",
                                                                               "getResetColorCode() matches the reset sequence");

                                                                 const std::set<std::string_view> distinctCodes{getColorCode(LogLevel::Trace),
                                                                                                                getColorCode(LogLevel::Debug),
                                                                                                                getColorCode(LogLevel::Info),
                                                                                                                getColorCode(LogLevel::Warn),
                                                                                                                getColorCode(LogLevel::Error),
                                                                                                                getColorCode(LogLevel::Fatal),
                                                                                                                getColorCode(LogLevel::Audit)};
                                                                 checks.expect(distinctCodes.size() == 7, "all seven levels have distinct color codes");
                                                                 checks.raise();
                                                             })
                                                       .Execute();
                                               }};

const speclab::Register complianceThresholdAtWarn{"isComplianceLevel() is true from WARN upward and false below it", "unit", [] {
                                                      return speclab::Test("loglevel-compliance-threshold")
                                                          .Then("TRACE, DEBUG and INFO are not compliance levels; WARN, ERROR, FATAL and AUDIT are",
                                                                [] {
                                                                    speclab::core::Checks checks;
                                                                    checks.expect(!isComplianceLevel(LogLevel::Trace), "TRACE is not a compliance level");
                                                                    checks.expect(!isComplianceLevel(LogLevel::Debug), "DEBUG is not a compliance level");
                                                                    checks.expect(!isComplianceLevel(LogLevel::Info), "INFO is not a compliance level");
                                                                    checks.expect(isComplianceLevel(LogLevel::Warn), "WARN is a compliance level");
                                                                    checks.expect(isComplianceLevel(LogLevel::Error), "ERROR is a compliance level");
                                                                    checks.expect(isComplianceLevel(LogLevel::Fatal), "FATAL is a compliance level");
                                                                    checks.expect(isComplianceLevel(LogLevel::Audit), "AUDIT is a compliance level");
                                                                    checks.raise();
                                                                })
                                                          .Execute();
                                                  }};

struct ThresholdState {
    SimpleLogger                   logger{"loglevel-threshold-test", false};
    std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
};

const speclab::Register sinkDeliversExactlyLevelsAtOrAboveThreshold{
    "A sink's minimum level admits exactly the levels at or above its threshold, across every level "
    "(complements FilteringSpec.cpp's single-threshold coverage)",
    "unit",
    [] {
        return speclab::Test<ThresholdState>("loglevel-sink-threshold-admits-at-or-above")
            .Given("a sink with minimum level ERROR attached to a logger with minimum level TRACE",
                   [](ThresholdState& s) {
                       s.sink->setMinLevel(LogLevel::Error);
                       s.logger.addSink(s.sink);
                       s.logger.setMinLevel(LogLevel::Trace);
                   })
            .When("every level from TRACE to AUDIT is logged once, in order, through the generic log() "
                  "(not logAudit(), whose bypass policy is covered separately in AuditPolicySpec.cpp)",
                  [](ThresholdState& s) {
                      for (auto level : allLevels) {
                          s.logger.log(level, toString(level));
                      }
                  })
            .Then("exactly ERROR, FATAL and AUDIT were delivered, in that order",
                  [](ThresholdState& s) {
                      speclab::core::Checks checks;
                      const auto            records = s.sink->records();
                      checks.expect(records.size() == 3, std::format("expected 3 records, got {}", records.size()));
                      if (records.size() == 3) {
                          checks.expect(records[0].level == LogLevel::Error, "1st delivered record is ERROR");
                          checks.expect(records[1].level == LogLevel::Fatal, "2nd delivered record is FATAL");
                          checks.expect(records[2].level == LogLevel::Audit, "3rd delivered record is AUDIT");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register consoleSinkRoutesAtOrAboveErrorToStderr{
    "ConsoleSink routes every level at or above ERROR to stderr, and the rest to stdout",
    "unit",
    [] {
        return speclab::Test("consolesink-stderr-routing")
            .Given("a ConsoleSink with colors disabled, stderr routing enabled (the default), and its "
                   "minimum level lowered to TRACE - ConsoleSink's own default minimum level is INFO "
                   "(Sink(core::LogLevel::Info) in its constructor), which would otherwise drop TRACE "
                   "and DEBUG before either stream saw them; that filtering is characterized separately "
                   "and is deliberately excluded here to isolate stdout/stderr routing",
                   [] {})
            .When("every level is written in turn, capturing stdout/stderr for each write", [] {})
            .Then("TRACE, DEBUG, INFO and WARN went to stdout; ERROR, FATAL and AUDIT went to stderr - "
                  "the sink's actual condition is level >= ERROR, so AUDIT (the highest level) routes "
                  "to stderr too, even though the constructor's doc comment only mentions ERROR and FATAL",
                  [] {
                      speclab::core::Checks checks;
                      ConsoleSink           sink{false, true};
                      sink.setMinLevel(LogLevel::Trace);
                      for (auto level : allLevels) {
                          StdStreamCapture capture;
                          LogRecord        record(level, "message body", "category");
                          sink.write(record);
                          const bool expectStderr = level >= LogLevel::Error;
                          const bool wentToStderr = !capture.err.str().empty();
                          const bool wentToStdout = !capture.out.str().empty();
                          checks.expect(wentToStderr == expectStderr,
                                        std::format("{} went to stderr: {} (expected {})", toString(level), wentToStderr, expectStderr));
                          checks.expect(wentToStdout == !expectStderr,
                                        std::format("{} went to stdout: {} (expected {})", toString(level), wentToStdout, !expectStderr));
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register consoleSinkRendersLevelUppercase{"ConsoleSink's formatted output renders the level in uppercase", "unit", [] {
                                                             return speclab::Test("consolesink-level-rendered-uppercase")
                                                                 .Given("a ConsoleSink with colors disabled", [] {})
                                                                 .When("an INFO record is written", [] {})
                                                                 .Then("the captured output contains the literal \"[INFO]\"",
                                                                       [] {
                                                                           ConsoleSink sink{false, false};
                                                                           LogRecord   record(LogLevel::Info, "message body", "category");
                                                                           std::string captured;
                                                                           {
                                                                               StdStreamCapture capture;
                                                                               sink.write(record);
                                                                               captured = capture.out.str();
                                                                           }
                                                                           if (!captured.contains("[INFO]")) {
                                                                               throw speclab::core::AssertionFailure(
                                                                                   std::format("expected the rendered output to contain \"[INFO]\", got: {}",
                                                                                               captured),
                                                                                   std::source_location::current());
                                                                           }
                                                                       })
                                                                 .Execute();
                                                         }};

}  // namespace
