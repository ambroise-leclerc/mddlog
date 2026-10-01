/** @brief Independent diagnostic masks, legacy rendering, facade lifetime and audit exclusion. */
import std;
import speclab;
import mddlog.adapter.textlogger;
import mddlog.core.loglevel;
import mddlog.core.auditevent;
import mddlog.sinks.auditsink;

#include "webfront/tooling/LoggerApi.hpp"

namespace {
using mddlog::adapter::TextLogger;
using mddlog::core::AuditEvent;
using mddlog::core::LogLevel;

template <typename T>
concept FacadeAcceptsEvent = requires(T event) { webfront::log::info(event); } || requires(T event) { webfront::log::warn(event); }
                             || requires(T event) { webfront::log::error(event); } || requires(T event) { webfront::log::debug(event); }
                             || requires(T event) { webfront::log::info("{}", event); } || requires(T event) { webfront::log::warn("{}", event); }
                             || requires(T event) { webfront::log::error("{}", event); } || requires(T event) { webfront::log::debug("{}", event); }
                             || requires(T event) { webfront::log::infoHex("audit", event); }
                             || requires(T event) { webfront::log::infoHex("audit", std::array<T, 1>{event}); }
                             || requires(T event) { webfront::log::set(event, true); } || requires(T event) { webfront::log::is(event); }
                             || requires(T event) { webfront::log::setLogLevel(event); } || requires(T event) { webfront::log::addSinks(event); }
                             || requires(T event) { webfront::log::removeSinks(event); }
                             || requires(T event) { webfront::log::detail::write(webfront::log::Info, event); }
                             || requires(T event) { webfront::log::detail::writeDebug(event, std::source_location::current()); }
                             || requires(T event) { webfront::log::detail::writeHex("audit", event); }
                             || requires(TextLogger& logger, T event) { logger.write(LogLevel::Info, event); }
                             || requires(TextLogger& logger, T event) { logger.writeDump(LogLevel::Info, "audit", event); };

template <typename T>
concept FacadeAcceptsSink = requires(T sink) { webfront::log::addSinks(sink); };

static_assert(!FacadeAcceptsEvent<AuditEvent>);
static_assert(!FacadeAcceptsSink<std::shared_ptr<mddlog::sinks::AuditSink>>);

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
                      checks.expect(!FacadeAcceptsEvent<AuditEvent>, "no facade API accepts or forwards an audit event");
                      logger.removeSink(handle);
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register facadeRendering{
    "WebFront facade: caller source, hex second write, formatting guards and independent handles",
    "unit",
    [] {
        return speclab::Test("webfront-facade-source-hex-and-handles")
            .Then("the reference non-module header preserves rendering and per-callback ownership",
                  [] {
                      speclab::core::Checks checks;
                      namespace log = webfront::log;
                      std::vector<std::string> lines;
                      std::size_t              otherWrites = 0;
                      log::setLogLevel(log::Debug);
                      const auto handles = log::addSinks(
                          [&](std::string_view line) {
                              lines.emplace_back(line);
                          },
                          [&](std::string_view) {
                              ++otherWrites;
                          });
                      static_assert(std::same_as<std::remove_cvref_t<decltype(handles)>, std::array<log::SinkHandle, 2>>);
                      const auto sourceLine = std::source_location::current().line() + 1;
                      log::debug("answer {}", 42);
                      checks.expect(lines.size() == 1 && lines[0].ends_with(" | answer 42"), "debug formatting");
                      checks.expect(lines[0].find(std::format("{:16}:{:4} |", "TextLoggerSpec.cpp", sourceLine)) != std::string::npos,
                                    "debug carries the actual caller file and line even on LLVM");
                      log::info("info {}", 42);
                      checks.expect(lines[1].ends_with(" | info 42") && lines[1].find("TextLoggerSpec.cpp") == std::string::npos,
                                    "info has no source location");
                      log::infoHex("bytes {literal}", std::array<unsigned char, 2>{0, 65});
                      checks.expect(lines.size() == 4 && lines[2].ends_with(" | bytes {literal}"), "infoHex text is not a format string");
                      checks.expect(lines[3] == "00000000  00 41                                            .A", "dump is a separate legacy-shaped write");
                      log::set(log::Info, false);
                      log::info("invalid {");
                      log::infoHex("suppressed", std::array<unsigned char, 1>{0});
                      checks.expect(lines.size() == 4, "disabled calls neither format nor emit either dump write");
                      log::removeSinks(handles[0]);
                      log::warn("remaining");
                      checks.expect(lines.size() == 4 && otherWrites == 5, "first multi-registration handle independently removes its callback");
                      log::removeSinks(handles[1]);
                      log::removeSinks(handles[0]);
                      log::set(log::Disabled, true);
                      checks.expect(!log::is(log::Disabled) && !log::is(log::Error) && !log::is(log::Debug), "Disabled masks every group");
                      checks.raise();
                  })
            .Execute();
    }};
const speclab::Register facadeAsciiDump{
    "WebFront facade: hex dump ASCII column is portable for every byte value",
    "unit",
    // Cover every byte, including the printable boundaries, DEL and the high-bit range.
    [] {
        return speclab::Test("webfront-facade-hex-portable-ascii")
            .Then("only printable ASCII survives, including at the DEL and high-byte boundaries",
                  [] {
                      speclab::core::Checks checks;
                      namespace log = webfront::log;
                      std::array<std::byte, 256> bytes{};
                      for (std::size_t index = 0; index < bytes.size(); ++index)
                          bytes.at(index) = static_cast<std::byte>(index);

                      std::vector<std::string> lines;
                      log::setLogLevel(log::Info);
                      const auto handle = log::addSinks([&](std::string_view line) {
                          lines.emplace_back(line);
                      });
                      log::infoHex("all bytes", bytes);
                      log::removeSinks(handle);
                      log::setLogLevel(log::Disabled);

                      checks.expect(lines.size() == 2 && lines.at(0).ends_with(" | all bytes"), "one message and one dump write");
                      const auto& dump = lines.at(1);
                      checks.expect(std::ranges::all_of(dump,
                                                        [](unsigned char value) {
                                                            return value < 128;
                                                        }),
                                    "dump contains only ASCII bytes");
                      const std::array<std::string_view, 16> expectedAscii{"................",
                                                                           "................",
                                                                           " !\"#$%&'()*+,-./",
                                                                           "0123456789:;<=>?",
                                                                           "@ABCDEFGHIJKLMNO",
                                                                           "PQRSTUVWXYZ[\\]^_",
                                                                           "`abcdefghijklmno",
                                                                           "pqrstuvwxyz{|}~.",
                                                                           "................",
                                                                           "................",
                                                                           "................",
                                                                           "................",
                                                                           "................",
                                                                           "................",
                                                                           "................",
                                                                           "................"};
                      checks.expect(dump.size() == (expectedAscii.size() * 75) + expectedAscii.size() - 1,
                                    "exactly sixteen full dump rows with fifteen newline separators");
                      for (std::size_t index = 0; index < expectedAscii.size(); ++index) {
                          const auto row = std::string_view(dump).substr(index * 76, 75);
                          checks.expect(row.starts_with(std::format("{:08x}", index * 16)) && row.ends_with(expectedAscii.at(index)),
                                        "row preserves layout and renders the exact printable ASCII subset");
                          if (index + 1 < expectedAscii.size())
                              checks.expect(dump.at((index * 76) + 75) == '\n', "each pair of rows has one newline separator");
                      }
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
