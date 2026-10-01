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

// The host TU maps the module-free snapshot into its sole producer's ring.
template <std::size_t Capacity>
void bind(RingLog<Capacity>& ring) {
    log::setRecordWriter([&ring](const log::DiagnosticRecord& record) {
        const auto level  = record.level == log::Debug   ? LogLevel::Debug
                            : record.level == log::Warn  ? LogLevel::Warn
                            : record.level == log::Error ? LogLevel::Error
                                                         : LogLevel::Info;
        auto       result = ring.tryWrite({.level         = level,
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
                          {{"jsFunction", "1", log::CallDirection::CppToJs, "1"},
                           {"jsFunction", "2", log::CallDirection::CppToJs, "1"},
                           {"jsFunction", "2", log::CallDirection::CppToJs, "1"},
                           {"jsFunction", "1", log::CallDirection::CppToJs, "1"},
                           {"cppFunction", "1", log::CallDirection::JsToCpp, "1"},
                           {"jsFunction", "2", log::CallDirection::CppToJs, "2"},
                           {"weblink", "2", log::CallDirection::CppToJs, "2"}}
                      };
                      for (const auto& value : contexts) {
                          // Mimic connection-owned data, destroyed before the consumer starts.
                          std::string       component(value.component), link(value.webLinkId), call(value.callId);
                          log::ContextScope scope({component, link, value.direction, call});
                          checks.expect(log::tryWrite(log::Info, "identical text with misleading ids 999").status == log::WriteStatus::Written, "admitted");
                      }
                      std::vector<LogRecord>             first, second;
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
                          const auto operation = std::string(contexts[i].direction == log::CallDirection::CppToJs ? "cpp-js:" : "js-cpp:")
                                                 + std::string(contexts[i].callId);
                          checks.expect(first[i].category == contexts[i].component && first[i].operationId == operation
                                            && first[i].correlationId == contexts[i].webLinkId,
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
                      std::string component(32, 'c'), call(25, 'o'), link(40, 'l');
                      {
                          log::ContextScope scope({component, link, log::CallDirection::CppToJs, call});
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
                              auto        invalid = log::Context{component, link, log::CallDirection::CppToJs, call};
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
class ContextSink : public mddlog::sinks::Sink {
public:
    ContextSink() : Sink(LogLevel::Trace) {}
    void write(const LogRecord& record) override {
        records.push_back(record);
    }
    void             flush() override {}
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
                          log::ContextScope scope({"socket", "12", log::CallDirection::JsToCpp, "3"});
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
}  // namespace
