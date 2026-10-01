/** @brief Emission context survives connection teardown and transport delivery. */
import std;
import speclab;
import mddlog.core.ring;
import mddlog.adapter.transportconsumer;
import mddlog.adapter.ringdrain;
import mddlog.sinks.sink;
import mddlog.adapter.logrecord;

#include "webfront/tooling/LoggerApi.hpp"

namespace {
namespace log = webfront::log;
using namespace mddlog::core;

/** @brief Bind the module-free emission snapshot to the host thread's sole producer ring. */
template <std::size_t Capacity>
void bind(RingLog<Capacity>& ring) {
    log::setRecordWriter([&ring](const log::DiagnosticRecord& record) {
        auto level = LogLevel::Info;
        switch (record.level) {
            case log::Debug:
                level = LogLevel::Debug;
                break;
            case log::Warn:
                level = LogLevel::Warn;
                break;
            case log::Error:
                level = LogLevel::Error;
                break;
            default:
                break;
        }
        auto result = ring.tryWrite({.level         = level,
                                     .time          = RawTime::available(record.time),
                                     .location      = record.location,
                                     .message       = record.message,
                                     .component     = record.component,
                                     .operationId   = record.operationId,
                                     .correlationId = record.correlationId});
        return result.admission() == Admission::Written;
    });
}

const speclab::Register contextCapture{
    "WebFront context: seven exchanges survive teardown and reach every transport",
    "unit",
    [] {
        return speclab::Test("webfront-context-capture-and-transports")
            .Then("context comes from fields, never text",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<8>            ring;
                      bind(ring);
                      log::setLogLevel(log::Debug);
                      const std::array<log::Context, 7> contexts{
                          {{.component = "jsFunction", .webLinkId = "1", .direction = log::CallDirection::CppToJs, .callId = "1"},
                           {.component = "jsFunction", .webLinkId = "2", .direction = log::CallDirection::CppToJs, .callId = "1"},
                           {.component = "jsFunction", .webLinkId = "2", .direction = log::CallDirection::CppToJs, .callId = "1"},
                           {.component = "jsFunction", .webLinkId = "1", .direction = log::CallDirection::CppToJs, .callId = "1"},
                           {.component = "cppFunction", .webLinkId = "1", .direction = log::CallDirection::JsToCpp, .callId = "1"},
                           {.component = "jsFunction", .webLinkId = "2", .direction = log::CallDirection::CppToJs, .callId = "2"},
                           {.component = "weblink", .webLinkId = "2", .direction = log::CallDirection::CppToJs, .callId = "2"}}
                      };
                      for (const auto& value : contexts) {
                          // Mimic connection-owned data, destroyed before the consumer starts.
                          std::string       component(value.component);
                          std::string       link(value.webLinkId);
                          std::string       call(value.callId);
                          log::ContextScope scope({.component = component, .webLinkId = link, .direction = value.direction, .callId = call});
                          checks.expect(log::tryWrite(log::Info, "identical text with misleading ids 999").status == log::WriteStatus::Written, "admitted");
                      }
                      std::vector<LogRecord>             first;
                      std::vector<LogRecord>             second;
                      mddlog::adapter::TransportConsumer consumer;
                      consumer.addRing(ring);
                      const auto one = consumer.addTransport([&](const LogRecord& record) {
                          first.push_back(record);
                      });
                      const auto two = consumer.addTransport([&](const LogRecord& record) {
                          second.push_back(record);
                      });
                      checks.expect(consumer.drainOnce() == contexts.size(), "all seven delivered after teardown");
                      checks.expect(first.size() == contexts.size() && second.size() == contexts.size(), "both transports retain fields");
                      for (std::size_t i = 0; i < first.size(); ++i) {
                          const auto operation = std::string(contexts.at(i).direction == log::CallDirection::CppToJs ? "cpp-js:" : "js-cpp:")
                                                 + std::string(contexts.at(i).callId);
                          checks.expect(first[i].category == contexts.at(i).component && first[i].operationId == operation
                                            && first[i].correlationId == contexts.at(i).webLinkId,
                                        "owned triplet independent of component and text");
                          checks.expect(second[i].category == first[i].category && second[i].operationId == operation
                                            && second[i].correlationId == first[i].correlationId,
                                        "same context for every transport");
                      }
                      consumer.removeTransport(one);
                      consumer.removeTransport(two);
                      log::setRecordWriter({});
                      log::setLogLevel(log::Disabled);
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register boundaries{
    "WebFront context: field limits, UTF-8, scopes and observable saturation",
    "unit",
    [] {
        return speclab::Test("webfront-context-boundaries")
            .Then("bounded records refuse identifiers without truncation",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<1>            ring;
                      bind(ring);
                      log::setLogLevel(log::Info);
                      std::string component(32, 'c');
                      std::string call(25, 'o');
                      std::string link(40, 'l');
                      {
                          log::ContextScope scope({.component = component, .webLinkId = link, .direction = log::CallDirection::CppToJs, .callId = call});
                          const std::string message = std::string(159, 'm') + "\xE2\x82\xAC";
                          const auto        result  = log::tryWrite(log::Info, message);
                          checks.expect(result.status == log::WriteStatus::Written && result.messageTruncated, "message is shortened");
                          const auto  view   = ring.drain();
                          const auto& record = view.first().front();
                          checks.expect(record.message().size() == 159 && record.truncated().message, "UTF-8 boundary and flag survive capture");
                          checks.expect(record.component().size() == 32 && record.operationId().size() == 32 && record.correlationId().size() == 40,
                                        "exact limits fit");
                          checks.expect(log::tryWrite(log::Info, "full").status == log::WriteStatus::RingFull, "saturation returned");
                          log::info("full legacy");
                          checks.expect(log::lastWriteOutcome().status == log::WriteStatus::RingFull && ring.refusalCount() == 2,
                                        "void API refusal observable");
                          for (auto field : {log::ContextField::Component, log::ContextField::OperationId, log::ContextField::CorrelationId}) {
                              auto invalid = log::Context{.component = component, .webLinkId = link, .direction = log::CallDirection::CppToJs, .callId = call};
                              std::string tooLong(field == log::ContextField::CorrelationId ? 41 : 33, 'x');
                              if (field == log::ContextField::Component)
                                  invalid.component = tooLong;
                              else if (field == log::ContextField::OperationId)
                                  invalid.callId = tooLong;
                              else
                                  invalid.webLinkId = tooLong;
                              log::ContextScope nested(invalid);
                              const auto        rejected = log::tryWrite(log::Info, "invalid");
                              checks.expect(rejected.status == log::WriteStatus::IdentifierTooLong && rejected.field == field,
                                            "identifier rejection precedes saturation");
                          }
                          checks.expect(ring.acknowledge(view, 1), "release captured record");
                          checks.expect(log::tryWrite(log::Info, "restored").status == log::WriteStatus::Written, "nested context restored");
                          const auto restored = ring.drain();
                          checks.expect(restored.first().front().correlationId() == link, "outer link restored");
                          checks.expect(ring.acknowledge(restored, 1), "release outer record");
                      }
                      checks.expect(log::tryWrite(log::Info, "outside").status == log::WriteStatus::Written, "outside context allowed");
                      const auto outside = ring.drain();
                      checks.expect(outside.first().front().component().empty() && outside.first().front().operationId().empty()
                                        && outside.first().front().correlationId().empty(),
                                    "no stale connection fields");
                      log::setRecordWriter({});
                      log::setLogLevel(log::Disabled);
                      checks.raise();
                  })
            .Execute();
    }};
/** @brief Retain diagnostic sink records to inspect their captured fields after draining. */
class ContextSink : public mddlog::sinks::Sink {
public:
    /** @brief Accept every diagnostic severity for context inspection. */
    ContextSink() : Sink(LogLevel::Trace) {}
    /** @brief Copy all fields before the adapter releases its transient record. */
    void write(const LogRecord& record) override {
        records.push_back(record);
    }
    /** @brief Complete immediately because this test sink has no buffered transport. */
    void flush() override {}
    /** @brief Identify the recording context sink. */
    std::string_view getName() const noexcept override {
        return "context";
    }
    std::vector<LogRecord> records;
};

const speclab::Register legacyCapture{
    "WebFront context: legacy calls and hex records retain context through diagnostic sinks",
    "unit",
    [] {
        return speclab::Test("webfront-context-legacy-sink-delivery")
            .Then("every legacy entry point captures the scope",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<8>            ring;
                      bind(ring);
                      log::setLogLevel(log::Debug);
                      const auto                       sink = std::make_shared<ContextSink>();
                      mddlog::adapter::RingSinkAdapter adapter;
                      adapter.addRing(ring);
                      adapter.addSink(sink);
                      std::uint_least32_t callerLine = 0;
                      {
                          log::ContextScope scope({.component = "socket", .webLinkId = "12", .direction = log::CallDirection::JsToCpp, .callId = "3"});
                          callerLine = std::source_location::current().line() + 1;
                          log::debug("debug");
                          log::info("info");
                          log::warn("warn");
                          log::error("error");
                          log::infoHex("dump", std::array<unsigned char, 3>{0x20, 0x7f, 0xff});
                          std::thread other([&checks] {
                              bool emptyContext = false;
                              log::setRecordWriter([&emptyContext](const log::DiagnosticRecord& record) {
                                  emptyContext = record.component.empty() && record.operationId.empty() && record.correlationId.empty();
                                  return true;
                              });
                              (void)log::tryWrite(log::Info, "other thread");
                              checks.expect(emptyContext, "another thread cannot inherit this connection or writer");
                              log::setRecordWriter({});
                          });
                          other.join();
                      }
                      checks.expect(adapter.drainOnce() == 6 && sink->records.size() == 6, "both dump records and four levels reach the sink");
                      for (const auto& record : sink->records)
                          checks.expect(record.category == "socket" && record.operationId == "js-cpp:3" && record.correlationId == "12",
                                        "all entries preserve context");
                      if (sink->records.size() == 6) {
                          checks.expect(sink->records[0].location.line() == callerLine
                                            && std::string_view(sink->records[0].location.file_name()).ends_with("WebFrontContextSpec.cpp"),
                                        "debug records preserve the actual caller");
                          checks.expect(sink->records[0].level == LogLevel::Debug && sink->records[1].level == LogLevel::Info
                                            && sink->records[2].level == LogLevel::Warn && sink->records[3].level == LogLevel::Error,
                                        "severity mapping survives capture");
                          checks.expect(sink->records[5].message.ends_with(" .."), "dump retains portable ASCII column");
                      }
                      log::setRecordWriter({});
                      log::setLogLevel(log::Disabled);
                      checks.raise();
                  })
            .Execute();
    }};
const speclab::Register hexLocationAndTruncation{
    "WebFront hex: caller location and explicit structured truncation preserve the complete text dump",
    "unit",
    [] {
        return speclab::Test("webfront-hex-caller-and-truncation")
            .Then("both records belong to the caller and truncation is observable",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<2>            ring;
                      bind(ring);
                      log::setLogLevel(log::Info);
                      std::vector<std::string>      lines;
                      const auto                    handle = log::addSink([&lines](std::string_view line) {
                          lines.emplace_back(line);
                      });
                      std::array<unsigned char, 64> bytes{};
                      bytes.fill('A');
                      const auto callerLine = std::source_location::current().line() + 1;
                      const auto result     = log::infoHex("large dump", bytes);
                      checks.expect(result.message.status == log::WriteStatus::Written && result.dump.status == log::WriteStatus::Written,
                                    "both structured records admitted");
                      checks.expect(!result.message.messageTruncated && result.dump.messageTruncated && log::lastWriteOutcome().messageTruncated,
                                    "dump truncation returned directly and retained in the summary");
                      const auto view = ring.drain();
                      checks.expect(view.size() == 2, "message and dump captured separately");
                      for (const auto& record : view.first()) {
                          checks.expect(record.location().line() == callerLine
                                            && std::string_view(record.location().file_name()).ends_with("WebFrontContextSpec.cpp"),
                                        "both records carry the infoHex caller location");
                      }
                      checks.expect(lines.size() == 2 && lines.back().find("00000030") != std::string::npos && lines.back().ends_with(std::string(16, 'A')),
                                    "legacy dump contains all four rows and their ASCII columns");
                      const auto& dump = view.first().back();
                      checks.expect(dump.truncated().message && dump.message().size() == messageCapacity, "owned ring dump retains its truncation flag");
                      if (lines.size() == 2)
                          checks.expect(dump.message() == std::string_view(lines.back()).substr(0, messageCapacity),
                                        "ring retains the bounded prefix of the full dump");
                      checks.expect(ring.acknowledge(view, view.size()), "release the large dump snapshot");
                      const auto messageTruncation = log::infoHex(std::string(161, 'm'), std::array<unsigned char, 1>{'A'});
                      checks.expect(messageTruncation.message.messageTruncated && !messageTruncation.dump.messageTruncated
                                        && log::lastWriteOutcome().messageTruncated,
                                    "a short dump cannot erase the preceding message's truncation flag");
                      log::removeSink(handle);
                      log::setRecordWriter({});
                      log::setLogLevel(log::Disabled);
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register independentTextDelivery{
    "WebFront channels: structured refusals preserve legacy diagnostics and expose partial hex admission",
    "unit",
    [] {
        return speclab::Test("webfront-independent-text-delivery")
            .Then("one free slot or a full ring never suppresses enabled text sinks",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<1>            ring;
                      bind(ring);
                      log::setLogLevel(log::Debug);
                      std::vector<std::string>           lines;
                      const auto                         handle = log::addSink([&lines](std::string_view line) {
                          lines.emplace_back(line);
                      });
                      const std::array<unsigned char, 1> bytes{'A'};
                      const auto                         partial = log::infoHex("partial", bytes);
                      checks.expect(partial.message.status == log::WriteStatus::Written && partial.dump.status == log::WriteStatus::RingFull,
                                    "the message fits but the dump reports refusal");
                      checks.expect(lines.size() == 2 && lines.front().ends_with(" | partial") && lines.back().ends_with(" A"),
                                    "both complete legacy writes survive partial admission");
                      checks.expect(log::lastWriteOutcome().status == log::WriteStatus::RingFull, "summary exposes partial admission");
                      const auto view = ring.drain();
                      checks.expect(view.size() == 1 && view.first().front().message() == "partial", "only the admitted message is retained");
                      checks.expect(log::tryWrite(log::Info, "full info").status == log::WriteStatus::RingFull, "tryWrite reports saturation");
                      log::debug("full debug");
                      const auto full = log::infoHex("full hex", bytes);
                      checks.expect(full.message.status == log::WriteStatus::RingFull && full.dump.status == log::WriteStatus::RingFull,
                                    "both full-ring refusals are returned");
                      checks.expect(lines.size() == 6 && lines[2].ends_with(" | full info") && lines[3].ends_with(" | full debug")
                                        && lines[4].ends_with(" | full hex") && lines[5].ends_with(" A"),
                                    "all enabled legacy APIs still deliver text");
                      checks.expect(ring.refusalCount() == 5, "each refused structured record increments saturation telemetry");
                      {
                          const std::string tooLong(33, 'c');
                          log::ContextScope scope({.component = tooLong});
                          checks.expect(log::tryWrite(log::Info, "invalid context").status == log::WriteStatus::IdentifierTooLong,
                                        "identifier refusal is independent of text delivery");
                      }
                      checks.expect(lines.size() == 7 && lines.back().ends_with(" | invalid context"), "context refusal preserves legacy text too");
                      log::setLogLevel(log::Disabled);
                      const auto filtered = log::infoHex("disabled", bytes);
                      checks.expect(filtered.message.status == log::WriteStatus::Filtered && filtered.dump.status == log::WriteStatus::Filtered
                                        && lines.size() == 7,
                                    "disabled calls deliver neither channel");
                      checks.expect(log::lastWriteOutcome().status == log::WriteStatus::IdentifierTooLong,
                                    "disabled legacy calls preserve the previous outcome");
                      log::removeSink(handle);
                      log::setRecordWriter({});
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
