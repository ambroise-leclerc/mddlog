/** @brief Adapter projections, lazy filtering and source forwarding for both existing loggers. */
import std;
import speclab;
import mddlog;
import mddlog.adapter.diagnosticbinding;
#include "../framework/RecordingSink.hpp"

namespace {
using namespace mddlog;
using mddlog::adapter::TextLogger;
using mddlog::spec::RecordingSink;

const speclab::Register textBinding{
    "Context: TextLogger binding filters factories before rendering and preserves caller source",
    "unit",
    [] {
        return speclab::Test("context-text-filter-source-and-temporaries")
            .Then("disabled groups do not evaluate a factory; enabled groups capture context",
                  [] {
                      speclab::core::Checks checks;
                      const auto            context = DiagnosticContext::create({.component = "pump", .operationId = "prime", .correlationId = "call-1"});
                      checks.expect(context.has_value(), "valid context");
                      if (context) {
                          TextLogger               text;
                          std::vector<std::string> lines;
                          const auto               handle = text.addSink([&](std::string_view line) {
                              lines.emplace_back(line);
                          });
                          DiagnosticBinding        logger(text, *context);
                          int                      constructions = 0;
                          const auto               factory       = [&] {
                              ++constructions;
                              return std::string("temporary");
                          };
                          logger.debugLazy(factory);
                          checks.expect(constructions == 0 && lines.empty(), "all groups disabled");
                          text.set(LogLevel::Debug, true);
                          const auto line = std::source_location::current().line() + 1;
                          logger.debugLazy(factory);
                          checks.expect(constructions == 1 && lines.size() == 1, "one factory invocation");
                          if (!lines.empty())
                              checks.expect(lines[0].ends_with("[pump:prime:call-1] temporary")
                                                && lines[0].find(std::format("{:16}:{:4}", "DiagnosticBindingSpec.cpp", line)) != std::string::npos,
                                            "caller source and every context field");
                          logger.logLazy(LogLevel::Warn, factory);
                          checks.expect(constructions == 1, "independent Warn group stays disabled");
                          logger.debugLazy([&] {
                              text.set(LogLevel::Debug, false);
                              ++constructions;
                              return std::string("raced disable");
                          });
                          checks.expect(constructions == 2 && lines.size() == 1, "reconfiguration after initial snapshot can suppress delivery");
                          text.set(LogLevel::Info, true);
                          logger.info(std::string("eager temporary"));
                          checks.expect(lines.size() == 2 && lines.back().ends_with("eager temporary"), "ordinary temporary copied synchronously");
                          text.removeSink(handle);
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register simpleBinding{
    "Context: SimpleLogger binding preserves structured context and threshold semantics",
    "unit",
    [] {
        return speclab::Test("context-simple-structured-fields-and-filter")
            .Then("context is not lost in the historical adapter",
                  [] {
                      speclab::core::Checks checks;
                      const auto            context = DiagnosticContext::create({.component = "pump", .operationId = "prime", .correlationId = "call-1"});
                      checks.expect(context.has_value(), "valid context");
                      if (context) {
                          SimpleLogger simple("context-test", false);
                          const auto   sink = std::make_shared<RecordingSink>();
                          simple.addSink(sink);
                          simple.setMinLevel(LogLevel::Info);
                          DiagnosticBinding logger(simple, *context);
                          int               constructions = 0;
                          const auto        factory       = [&] {
                              ++constructions;
                              return std::string("lazy diagnostic");
                          };
                          logger.debugLazy(factory);
                          checks.expect(!simple.is(LogLevel::Debug) && constructions == 0, "threshold filters before construction");
                          const auto line = std::source_location::current().line() + 1;
                          logger.logLazy(LogLevel::Info, factory);
                          auto records = sink->records();
                          checks.expect(records.size() == 1 && constructions == 1, "one delivery");
                          if (!records.empty()) {
                              checks.expect(records[0].category == "pump" && records[0].operationId == "prime" && records[0].correlationId == "call-1",
                                            "structured projection");
                              checks.expect(records[0].message == "lazy diagnostic" && records[0].location.line() == line && records[0].timeAvailable,
                                            "message, source and adapter clock");
                          }
                          simple.setEnabled(false);
                          logger.logLazy(LogLevel::Fatal, factory);
                          checks.expect(constructions == 1 && !logger.is(LogLevel::Fatal), "global enabled flag is respected");
                          simple.setEnabled(true);
                          bool propagated = false;
                          try {
                              logger.logLazy(LogLevel::Info, []() -> std::string {
                                  throw std::runtime_error("factory failed");
                              });
                          } catch (const std::runtime_error&) {
                              propagated = true;
                          }
                          checks.expect(propagated && sink->size() == 1, "factory exception creates no record");
                          simple.log(LogLevel::Info, "legacy", {});
                          records = sink->records();
                          checks.expect(records.size() == 2 && records.back().category.empty(), "legacy empty category remains unambiguous");
                      }
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
