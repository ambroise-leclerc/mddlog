/** @brief Independent diagnostic masks, legacy rendering, callback lifetime and audit exclusion. */
import std;
import speclab;
import mddlog.adapter.textlogger;
import mddlog.core.loglevel;
import mddlog.core.auditevent;
import mddlog.sinks.auditsink;

namespace {
using mddlog::adapter::TextLogger;
using mddlog::core::AuditEvent;
using mddlog::core::LogLevel;

template <typename T>
concept TextLoggerAcceptsEvent = requires(TextLogger& logger, T event) { logger.write(LogLevel::Info, event); }
                                 || requires(TextLogger& logger, T event) { logger.writeDump(LogLevel::Info, "audit", event); }
                                 || requires(TextLogger& logger, T event) { logger.writeDump(LogLevel::Info, event, "dump"); }
                                 || requires(TextLogger& logger, T event) { logger.set(event, true); }
                                 || requires(TextLogger& logger, T event) { logger.is(event); };

template <typename T>
concept TextLoggerAcceptsSink = requires(TextLogger& logger, T sink) { logger.addSink(sink); };

static_assert(!TextLoggerAcceptsEvent<AuditEvent>);
static_assert(!TextLoggerAcceptsSink<std::shared_ptr<mddlog::sinks::AuditSink>>);

const speclab::Register maskAndMapping{
    "TextLogger: independent masks and explicit diagnostic aliases exclude audit",
    "unit",
    [] {
        return speclab::Test("text-logger-mask-mapping-and-audit-exclusion")
            .Then("all groups start disabled and Warn can stay off while Error and Fatal stay on",
                  [] {
                      speclab::core::Checks    checks;
                      TextLogger               logger;
                      std::vector<std::string> lines;
                      const auto               handle = logger.addSink([&](std::string_view line) {
                          lines.emplace_back(line);
                      });
                      for (auto level : {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal}) {
                          checks.expect(!logger.is(level), "every diagnostic group starts disabled");
                          logger.write(level, "disabled");
                      }
                      checks.expect(lines.empty(), "no implicit console or audit delivery");
                      logger.set(LogLevel::Error, true);
                      logger.write(LogLevel::Warn, "suppressed");
                      logger.write(LogLevel::Error, "error");
                      logger.write(LogLevel::Fatal, "fatal");
                      checks.expect(lines.size() == 2 && lines[0].starts_with("[E]") && lines[1].starts_with("[E]"), "Error and Fatal share E");
                      logger.set(LogLevel::Debug, true);
                      logger.write(LogLevel::Trace, "trace");
                      logger.write(LogLevel::Debug, "debug");
                      logger.set(LogLevel::Info, true);
                      logger.write(LogLevel::Info, "info");
                      logger.set(LogLevel::Warn, true);
                      logger.write(LogLevel::Warn, "warn");
                      checks.expect(lines.size() == 6 && lines[2].starts_with("[D]") && lines[3].starts_with("[D]") && lines[4].starts_with("[I]")
                                        && lines[5].starts_with("[W]"),
                                    "all mappings are explicit");
                      logger.disableAll();
                      logger.write(LogLevel::Fatal, "disabled again");
                      checks.expect(lines.size() == 6, "disabling all also masks aliases");
                      checks.expect(!TextLoggerAcceptsEvent<AuditEvent>, "no TextLogger API accepts or forwards an audit event");
                      logger.removeSink(handle);
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register rendering{
    "TextLogger: caller source, separate dump write, disabled groups and independent handles",
    "unit",
    [] {
        return speclab::Test("text-logger-source-dump-and-handles")
            .Then("rendering keeps the legacy shape and each callback has its own registration",
                  [] {
                      speclab::core::Checks    checks;
                      TextLogger               logger;
                      std::vector<std::string> lines;
                      std::size_t              otherWrites = 0;
                      const auto               first       = logger.addSink([&](std::string_view line) {
                          lines.emplace_back(line);
                      });
                      const auto               second      = logger.addSink([&](std::string_view) {
                          ++otherWrites;
                      });
                      logger.set(LogLevel::Debug, true);
                      logger.set(LogLevel::Info, true);
                      const auto location = std::source_location::current();
                      logger.write(LogLevel::Debug, "answer 42", location);
                      checks.expect(lines.size() == 1 && lines[0].starts_with("[D] ") && lines[0].ends_with(" | answer 42"),
                                    "debug line keeps level character and message position");
                      checks.expect(lines[0].find(std::format("{:16}:{:4} |", "TextLoggerSpec.cpp", location.line())) != std::string::npos,
                                    "a supplied location renders the caller's file basename and line");
                      logger.write(LogLevel::Info, "info 42");
                      checks.expect(lines.size() == 2 && lines[1].ends_with(" | info 42") && lines[1].find("TextLoggerSpec.cpp") == std::string::npos,
                                    "without a location no file is rendered");
                      logger.writeDump(LogLevel::Info, "bytes {literal}", "00000000  00 41");
                      checks.expect(lines.size() == 4 && lines[2].starts_with("[I] ") && lines[2].ends_with(" | bytes {literal}"),
                                    "dump message is rendered verbatim, not as a format string");
                      checks.expect(lines[3] == "00000000  00 41", "dump is a separate, unprefixed write");
                      logger.set(LogLevel::Info, false);
                      logger.write(LogLevel::Info, "suppressed");
                      logger.writeDump(LogLevel::Info, "suppressed", "00000000  00");
                      checks.expect(lines.size() == 4, "a disabled group emits neither the message nor the dump");
                      logger.removeSink(first);
                      logger.write(LogLevel::Debug, "remaining");
                      checks.expect(lines.size() == 4 && otherWrites == 5, "removing one handle leaves the other callback registered");
                      logger.removeSink(second);
                      logger.removeSink(first);
                      logger.disableAll();
                      checks.expect(!logger.is(LogLevel::Error) && !logger.is(LogLevel::Debug), "disableAll masks every group");
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
