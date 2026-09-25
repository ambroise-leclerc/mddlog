/**
 * @brief Governed record capacity, admission, ownership, time and UTF-8 contract (issue #35).
 */
import std;
import speclab;
import mddlog.core.record;

namespace {

using mddlog::core::Admission;
using mddlog::core::GovernedRecord;
using mddlog::core::IdentifierField;
using mddlog::core::LogLevel;
using mddlog::core::RawTime;
using mddlog::core::RecordInput;
using mddlog::core::RefusalReason;
using mddlog::core::TimeAvailability;

constexpr auto epoch = std::chrono::sys_time<std::chrono::nanoseconds>{};

static_assert(std::is_trivially_copyable_v<GovernedRecord>);
static_assert(std::is_trivially_copyable_v<RawTime>);
static_assert(!std::is_aggregate_v<RawTime>);
static_assert(!std::is_default_constructible_v<RawTime>);
static_assert(!std::is_constructible_v<RawTime, std::chrono::sys_time<std::chrono::nanoseconds>, TimeAvailability>);
static_assert(mddlog::core::messageCapacity == 160);
static_assert(mddlog::core::componentCapacity == 32);
static_assert(mddlog::core::operationIdCapacity == 32);
static_assert(mddlog::core::correlationIdCapacity == 40);
static_assert(sizeof(GovernedRecord) <= mddlog::core::maximumStandardRecordBytes);
static_assert(noexcept(std::declval<GovernedRecord&>().assign(std::declval<const RecordInput&>())));

consteval bool recordAssignmentIsConstantEvaluable() {
    GovernedRecord    record;
    const RecordInput input{.level         = LogLevel::Info,
                            .time          = RawTime::unavailable(),
                            .location      = std::source_location::current(),
                            .message       = "constant",
                            .component     = "core",
                            .operationId   = "test",
                            .correlationId = "id"};
    const auto        result = record.assign(input);
    return result.admission() == Admission::Written && record.message() == "constant" && record.time().availability() == TimeAvailability::Unavailable;
}
static_assert(recordAssignmentIsConstantEvaluable());

[[nodiscard]] RecordInput inputWith(std::string_view message       = "message",
                                    std::string_view component     = "component",
                                    std::string_view operationId   = "operation",
                                    std::string_view correlationId = "correlation") noexcept {
    return {.level         = LogLevel::Warn,
            .time          = RawTime::available(epoch),
            .location      = std::source_location::current(),
            .message       = message,
            .component     = component,
            .operationId   = operationId,
            .correlationId = correlationId};
}

[[nodiscard]] bool sameRecord(const GovernedRecord& lhs, const GovernedRecord& rhs) noexcept {
    return lhs.level() == rhs.level() && lhs.time().availability() == rhs.time().availability() && lhs.time().value() == rhs.time().value()
           && lhs.location().line() == rhs.location().line() && std::string_view(lhs.location().file_name()) == std::string_view(rhs.location().file_name())
           && lhs.message() == rhs.message() && lhs.component() == rhs.component() && lhs.operationId() == rhs.operationId()
           && lhs.correlationId() == rhs.correlationId() && lhs.truncated().message == rhs.truncated().message;
}

const speclab::Register defaultInputCapturesEmissionSiteAndInfoLevel{
    "RecordInput defaults to Info and captures the caller's source location",
    "unit",
    [] {
        return speclab::Test("governed-record-input-defaults")
            .Then("omitting level and location produces Info and a nonempty location from this call site",
                  [] {
                      speclab::core::Checks checks;
                      const RecordInput     input{.time          = RawTime::unavailable(),
                                                  .message       = "defaulted",
                                                  .component     = "core",
                                                  .operationId   = "test",
                                                  .correlationId = "id"};
                      const auto            site = std::source_location::current();
                      checks.expect(input.level == LogLevel::Info, "omitted level defaults to Info");
                      checks.expect(input.location.line() != 0, "omitted location has a real line");
                      checks.expect(std::string_view(input.location.file_name()) == std::string_view(site.file_name()),
                                    "omitted location names the producer's file");
                      GovernedRecord record;
                      checks.expect(record.assign(input).admission() == Admission::Written, "defaulted input is admitted");
                      checks.expect(record.level() == LogLevel::Info, "record retains Info level");
                      checks.expect(record.location().line() == input.location.line(), "record retains captured line");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register recordAcceptsExactCapacitiesAndCapturesValues{
    "GovernedRecord accepts each field at its exact byte capacity and owns emission context",
    "unit",
    [] {
        return speclab::Test("governed-record-exact-capacities")
            .Then("all four exact-capacity fields are retained whole with caller-supplied level, time and location",
                  [] {
                      speclab::core::Checks checks;
                      std::string           message(mddlog::core::messageCapacity, 'm');
                      std::string           component(mddlog::core::componentCapacity, 'c');
                      std::string           operation(mddlog::core::operationIdCapacity, 'o');
                      std::string           correlation(mddlog::core::correlationIdCapacity, 'r');
                      const auto            site = std::source_location::current();
                      const RecordInput     input{.level         = LogLevel::Audit,
                                                  .time          = RawTime::available(epoch + std::chrono::nanoseconds{42}),
                                                  .location      = site,
                                                  .message       = message,
                                                  .component     = component,
                                                  .operationId   = operation,
                                                  .correlationId = correlation};
                      GovernedRecord        record;
                      const auto            result = record.assign(input);
                      checks.expect(result.admission() == Admission::Written, "exact-capacity values are admitted");
                      checks.expect(!result.truncated().message, "exact-capacity message is not truncated");
                      checks.expect(record.message() == message, "message is retained whole");
                      checks.expect(record.component() == component, "component is retained whole");
                      checks.expect(record.operationId() == operation, "operation is retained whole");
                      checks.expect(record.correlationId() == correlation, "correlation is retained whole");
                      checks.expect(record.level() == LogLevel::Audit, "level is captured");
                      checks.expect(record.time().value() == epoch + std::chrono::nanoseconds{42}, "host time is captured");
                      checks.expect(record.location().line() == site.line(), "emission source location is captured");

                      message[0]     = 'x';
                      component[0]   = 'x';
                      operation[0]   = 'x';
                      correlation[0] = 'x';
                      checks.expect(record.message().front() == 'm', "message bytes are owned");
                      checks.expect(record.component().front() == 'c', "component bytes are owned");
                      checks.expect(record.operationId().front() == 'o', "operation bytes are owned");
                      checks.expect(record.correlationId().front() == 'r', "correlation bytes are owned");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register overlongIdentifiersRefuseWithoutChangingRecord{
    "GovernedRecord refuses each overlong identifier and leaves the previous record unchanged",
    "unit",
    [] {
        return speclab::Test("governed-record-identifier-refusal")
            .Then("each identifier names its own field, while multiple failures select the first field",
                  [] {
                      speclab::core::Checks checks;
                      GovernedRecord        record;
                      const std::string     baselineMessage(mddlog::core::messageCapacity + 1, 'b');
                      checks.expect(record.assign(inputWith(baselineMessage)).truncated().message, "baseline record is written with truncation");
                      const GovernedRecord before = record;
                      const std::string    component(mddlog::core::componentCapacity + 1, 'c');
                      const std::string    operation(mddlog::core::operationIdCapacity + 1, 'o');
                      const std::string    correlation(mddlog::core::correlationIdCapacity + 1, 'r');

                      const auto componentResult = record.assign(inputWith(baselineMessage, component));
                      checks.expect(componentResult.refusal()->reason == RefusalReason::IdentifierTooLong, "component overflow is refused");
                      checks.expect(componentResult.refusal()->field == IdentifierField::Component, "component is named");
                      checks.expect(!componentResult.truncated().message, "refusal reports no truncation");
                      checks.expect(sameRecord(record, before), "component refusal leaves the record unchanged");

                      const auto operationResult = record.assign(inputWith("new", "ok", operation));
                      checks.expect(operationResult.refusal()->reason == RefusalReason::IdentifierTooLong, "operation overflow is refused");
                      checks.expect(operationResult.refusal()->field == IdentifierField::OperationId, "operation is named");
                      checks.expect(!operationResult.truncated().message, "operation refusal reports no truncation");
                      checks.expect(sameRecord(record, before), "operation refusal leaves the record unchanged");

                      const auto correlationResult = record.assign(inputWith("new", "ok", "ok", correlation));
                      checks.expect(correlationResult.refusal()->reason == RefusalReason::IdentifierTooLong, "correlation overflow is refused");
                      checks.expect(correlationResult.refusal()->field == IdentifierField::CorrelationId, "correlation is named");
                      checks.expect(!correlationResult.truncated().message, "correlation refusal reports no truncation");
                      checks.expect(sameRecord(record, before), "correlation refusal leaves the record unchanged");

                      const auto multipleResult = record.assign(inputWith("new", component, operation, correlation));
                      checks.expect(multipleResult.refusal()->field == IdentifierField::Component, "first overlong identifier wins");
                      checks.expect(sameRecord(record, before), "multiple failures leave the record unchanged");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register messageTruncationKeepsUtf8Whole{
    "GovernedRecord truncates a descriptive message without splitting a UTF-8 sequence",
    "unit",
    [] {
        return speclab::Test("governed-record-message-utf8-truncation")
            .Then("a four-byte character crossing the 160-byte boundary is dropped whole and flagged",
                  [] {
                      speclab::core::Checks checks;
                      const std::string     crossing = std::string(mddlog::core::messageCapacity - 2, 'a') + "\360\237\230\200";
                      GovernedRecord        record;
                      const auto            result = record.assign(inputWith(crossing));
                      checks.expect(result.admission() == Admission::Written, "long message is admitted");
                      checks.expect(result.truncated().message, "result flags truncation");
                      checks.expect(record.truncated().message, "record carries truncation flag");
                      checks.expect(record.message() == std::string(mddlog::core::messageCapacity - 2, 'a'), "partial UTF-8 sequence is not stored");

                      const std::string exactThenMore = std::string(mddlog::core::messageCapacity - 4, 'a') + "\360\237\230\200" + "x";
                      const auto        secondResult  = record.assign(inputWith(exactThenMore));
                      checks.expect(secondResult.truncated().message, "extra byte is reported as truncation");
                      checks.expect(record.message() == exactThenMore.substr(0, mddlog::core::messageCapacity), "whole boundary character is retained");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register unavailableTimeIsAdmittedAndMalformedTimeUnrepresentable{
    "GovernedRecord admits unavailable host time and RawTime has no malformed state",
    "unit",
    [] {
        return speclab::Test("governed-record-host-time-states")
            .Then("both RawTime factories yield admitted records, including the no-clock state",
                  [] {
                      speclab::core::Checks checks;
                      GovernedRecord        record;
                      RecordInput           input = inputWith();
                      input.time                  = RawTime::unavailable();
                      const auto noClock          = record.assign(input);
                      checks.expect(noClock.admission() == Admission::Written, "unavailable time is admitted");
                      checks.expect(record.time().availability() == TimeAvailability::Unavailable, "record preserves unavailable state");
                      input.time           = RawTime::available(epoch);
                      const auto available = record.assign(input);
                      checks.expect(available.admission() == Admission::Written, "available time is admitted");
                      checks.expect(record.time().availability() == TimeAvailability::Available, "record preserves available state");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
