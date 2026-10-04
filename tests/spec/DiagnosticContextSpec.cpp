/** @brief Owned contexts, typed failures, true caller source and governed binding independence. */
import std;
import speclab;
import mddlog.core.governedbinding;

namespace {
using namespace mddlog::core;

static_assert(!std::is_default_constructible_v<DiagnosticContext>);
static_assert(!std::is_constructible_v<GovernedBinding<2>, RingLog<2>&&, DiagnosticContext>);
static_assert(noexcept(DiagnosticContext::create({})));

const speclab::Register ownedContexts{
    "Context: diagnostic snapshots own temporaries and nested operations are independent",
    "unit",
    [] {
        return speclab::Test("context-diagnostic-owned-and-nested")
            .Then("derivation preserves the parent and copies bounded identities",
                  [] {
                      speclab::core::Checks checks;
                      auto parent = DiagnosticContext::create({.component = std::string("pump"), .operationId = "parent", .correlationId = "root"});
                      checks.expect(parent.has_value(), "parent created");
                      if (parent) {
                          auto child   = parent->withOperation({.operationId = std::string("prime"), .correlationId = std::string("call-1")});
                          auto sibling = parent->withOperation({.operationId = "stop", .correlationId = "call-2"});
                          checks.expect(child.has_value() && sibling.has_value(), "children created");
                          if (child && sibling) {
                              auto nested = child->withOperation({.operationId = "verify", .correlationId = "call-3"});
                              checks.expect(nested && nested->component() == "pump" && nested->operationId() == "verify", "nested copy");
                              checks.expect(child->operationId() == "prime" && child->correlationId() == "call-1", "child unchanged");
                              checks.expect(sibling->operationId() == "stop" && sibling->correlationId() == "call-2", "sibling independent");
                          }
                          checks.expect(parent->operationId() == "parent" && parent->correlationId() == "root", "parent unchanged");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register identifierFailures{
    "Context: diagnostic identifier limits produce exact typed failures",
    "unit",
    [] {
        return speclab::Test("context-diagnostic-identifier-failures")
            .Then("each field fits exactly or refuses without modifying the parent",
                  [] {
                      speclab::core::Checks checks;
                      const auto            exact = DiagnosticContext::create({.component     = std::string(componentCapacity, 'c'),
                                                                               .operationId   = std::string(operationIdCapacity, 'o'),
                                                                               .correlationId = std::string(correlationIdCapacity, 'r')});
                      checks.expect(exact.has_value(), "all exact capacity values accepted");
                      const auto badComponent = DiagnosticContext::create(
                          {.component = std::string(componentCapacity + 1, 'c'), .operationId = std::string(operationIdCapacity + 1, 'o')});
                      const auto badOperation   = DiagnosticContext::create({.operationId = std::string(operationIdCapacity + 1, 'o')});
                      const auto badCorrelation = DiagnosticContext::create({.correlationId = std::string(correlationIdCapacity + 1, 'r')});
                      checks.expect(!badComponent && badComponent.error().reason == RefusalReason::IdentifierTooLong
                                        && badComponent.error().field == IdentifierField::Component,
                                    "first field wins");
                      checks.expect(!badOperation && badOperation.error().field == IdentifierField::OperationId, "operation refused");
                      checks.expect(!badCorrelation && badCorrelation.error().field == IdentifierField::CorrelationId, "correlation refused");
                      if (exact) {
                          const auto invalidChild = exact->withOperation({.operationId = std::string(operationIdCapacity + 1, 'x')});
                          checks.expect(!invalidChild && exact->operationId().size() == operationIdCapacity, "failed derivation leaves parent intact");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register governedSource{
    "Context: governed binding preserves time, source, temporary messages and explicit refusal",
    "unit",
    [] {
        return speclab::Test("context-governed-source-time-and-refusal")
            .Then("emission captures the business caller and retains existing truncation",
                  [] {
                      speclab::core::Checks checks;
                      const auto            context = DiagnosticContext::create({.component = "pump", .operationId = "prime", .correlationId = "call-1"});
                      checks.expect(context.has_value(), "valid context");
                      if (context) {
                          RingLog<2>      ring;
                          GovernedBinding logger(ring, *context);
                          const auto      time  = RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{123}});
                          const auto      line  = std::source_location::current().line() + 1;
                          const auto      first = logger.info(time, std::string("temporary"));
                          checks.expect(first.admission() == Admission::Written, "first admitted");
                          const auto shortened = logger.warn(RawTime::unavailable(), std::string(messageCapacity - 1, 'x') + "\xC3\xA9");
                          checks.expect(shortened.admission() == Admission::Written && shortened.truncated().message, "UTF-8 truncation visible");
                          const auto refused = logger.error(time, "full");
                          const auto failure = refused.refusal();
                          checks.expect(failure && failure->reason == RefusalReason::RingFull && ring.refusalCount() == 1, "saturation preserved");
                          const auto view = ring.drain();
                          if (view.size() == 2) {
                              const auto& record = view.first()[0];
                              checks.expect(record.location().line() == line
                                                && std::string_view(record.location().file_name()).ends_with("DiagnosticContextSpec.cpp"),
                                            "true caller");
                              checks.expect(record.message() == "temporary" && record.component() == "pump" && record.operationId() == "prime"
                                                && record.correlationId() == "call-1",
                                            "owned event and context");
                              checks.expect(record.time().value() == time.value() && view.first()[1].time().availability() == TimeAvailability::Unavailable,
                                            "time varies per event");
                              checks.expect(view.first()[1].message().size() == messageCapacity - 1 && view.first()[1].level() == LogLevel::Warn,
                                            "no split UTF-8 sequence");
                          } else
                              checks.expect(false, "two records expected");
                          checks.expect(ring.acknowledge(view, view.size()), "consumer releases independently");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register producerIsolation{
    "Context: copies used by separate concurrent diagnostic producers remain isolated",
    "integration",
    [] {
        return speclab::Test("context-diagnostic-producer-isolation")
            .Then("one ring per producer preserves both operation identities",
                  [] {
                      speclab::core::Checks checks;
                      const auto            context = DiagnosticContext::create({.component = "pump"});
                      checks.expect(context.has_value(), "component exists");
                      if (context) {
                          const auto firstContext  = context->withOperation({.operationId = "first", .correlationId = "call-1"});
                          const auto secondContext = context->withOperation({.operationId = "second", .correlationId = "call-2"});
                          if (firstContext && secondContext) {
                              RingLog<32>       firstRing;
                              RingLog<32>       secondRing;
                              std::atomic<bool> allWritten{true};
                              auto              run = [&](auto& ring, DiagnosticContext value) {
                                  GovernedBinding logger(ring, value);
                                  for (int i = 0; i < 32; ++i)
                                      if (logger.info(RawTime::unavailable(), "event").admission() != Admission::Written)
                                          allWritten.store(false);
                              };
                              std::jthread first([&] {
                                  run(firstRing, *firstContext);
                              });
                              std::jthread second([&] {
                                  run(secondRing, *secondContext);
                              });
                              first.join();
                              second.join();
                              checks.expect(allWritten.load(), "both producers admit all events");
                              for (const auto& record : firstRing.drain().first())
                                  checks.expect(record.operationId() == "first" && record.correlationId() == "call-1", "first isolated");
                              for (const auto& record : secondRing.drain().first())
                                  checks.expect(record.operationId() == "second" && record.correlationId() == "call-2", "second isolated");
                          } else
                              checks.expect(false, "operation creation failed");
                      }
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
