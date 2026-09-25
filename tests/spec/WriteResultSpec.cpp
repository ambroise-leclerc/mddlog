/**
 * @brief Admission and truncation result contract (ADR-001 Decision 3, issue #34).
 */
import std;
import speclab;
import mddlog.core.writeresult;

namespace {

using mddlog::core::Admission;
using mddlog::core::IdentifierField;
using mddlog::core::RefusalReason;
using mddlog::core::TruncatedFields;
using mddlog::core::WriteResult;

template <typename T>
concept MutableAdmission = requires(T& result) { result.admission = Admission::Refused; };

template <typename T>
concept MutableRefusal = requires(T& result) { result.refusal = mddlog::core::Refusal{}; };

template <typename T>
concept MutableTruncation = requires(T& result) { result.truncated = TruncatedFields{}; };

static_assert(std::is_trivially_copyable_v<WriteResult>);
static_assert(!MutableAdmission<WriteResult>);
static_assert(!MutableRefusal<WriteResult>);
static_assert(!MutableTruncation<WriteResult>);
static_assert(noexcept(WriteResult::written()));
static_assert(noexcept(WriteResult::refused({.reason = RefusalReason::RingFull, .field = IdentifierField::Component})));
static_assert(noexcept(WriteResult::written().admission()));
static_assert(noexcept(WriteResult::written().refusal()));
static_assert(noexcept(WriteResult::written().truncated()));

const speclab::Register admittedWriteReportsTruncationIndependently{
    "An admitted governed write reports descriptive-field truncation independently of admission",
    "unit",
    [] {
        return speclab::Test("writeresult-written-truncation")
            .Then("a whole message and a shortened message are both Written, with distinct truncation flags",
                  [] {
                      speclab::core::Checks checks;
                      constexpr WriteResult whole     = WriteResult::written();
                      constexpr WriteResult shortened = WriteResult::written(TruncatedFields{.message = true});
                      checks.expect(whole.admission() == Admission::Written, "whole message is admitted");
                      checks.expect(!whole.refusal().has_value(), "whole message has no refusal reason");
                      checks.expect(!whole.truncated().message, "whole message is not marked truncated");
                      checks.expect(shortened.admission() == Admission::Written, "shortened message is admitted");
                      checks.expect(!shortened.refusal().has_value(), "shortened message has no refusal reason");
                      checks.expect(shortened.truncated().message, "shortened message is marked truncated");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register refusedWriteReportsReasonWithoutTruncation{
    "A refused governed write reports its reason and no truncation",
    "unit",
    [] {
        return speclab::Test("writeresult-refused-reasons")
            .Then("RingFull, IdentifierTooLong for each identifier, and MalformedTime are distinct refusals with no truncated field",
                  [] {
                      speclab::core::Checks checks;
                      constexpr WriteResult full = WriteResult::refused({.reason = RefusalReason::RingFull, .field = IdentifierField::Component});
                      checks.expect(full.admission() == Admission::Refused, "full ring refuses the write");
                      checks.expect(full.refusal()->reason == RefusalReason::RingFull, "ring-full reason is preserved");
                      checks.expect(!full.truncated().message, "ring-full refusal has no truncation");

                      constexpr std::array fields{IdentifierField::Component, IdentifierField::OperationId, IdentifierField::CorrelationId};
                      for (const IdentifierField field : fields) {
                          const WriteResult tooLong = WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = field});
                          checks.expect(tooLong.admission() == Admission::Refused, "overlong identifier refuses the write");
                          checks.expect(tooLong.refusal()->reason == RefusalReason::IdentifierTooLong, "identifier reason is preserved");
                          checks.expect(tooLong.refusal()->field == field, "the offending identifier field is preserved");
                          checks.expect(!tooLong.truncated().message, "identifier refusal has no truncation");
                      }

                      constexpr WriteResult malformed = WriteResult::refused({.reason = RefusalReason::MalformedTime, .field = IdentifierField::Component});
                      checks.expect(malformed.admission() == Admission::Refused, "malformed time refuses the write");
                      checks.expect(malformed.refusal()->reason == RefusalReason::MalformedTime, "malformed-time reason is preserved");
                      checks.expect(!malformed.truncated().message, "malformed-time refusal has no truncation");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register resultStateRemainsConsistentAfterAssignment{
    "Assigning another result keeps refusal and truncation states consistent",
    "unit",
    [] {
        return speclab::Test("writeresult-assignment-preserves-invariant")
            .Then("replacing a truncated write with a refusal clears truncation, and accessors do not expose mutable state",
                  [] {
                      speclab::core::Checks checks;
                      WriteResult           result = WriteResult::written(TruncatedFields{.message = true});
                      result                       = WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::OperationId});
                      checks.expect(result.admission() == Admission::Refused, "replacement is refused");
                      checks.expect(result.refusal()->field == IdentifierField::OperationId, "replacement keeps its offending field");
                      checks.expect(!result.truncated().message, "replacement has no stale truncation");

                      auto observedRefusal       = result.refusal();
                      observedRefusal->reason    = RefusalReason::RingFull;
                      auto observedTruncation    = result.truncated();
                      observedTruncation.message = true;
                      checks.expect(result.refusal()->reason == RefusalReason::IdentifierTooLong, "editing a returned refusal does not change the result");
                      checks.expect(!result.truncated().message, "editing returned flags does not change the result");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register defaultRefusalInitializesBothFields{"A default refusal initializes both its reason and identifier field", "unit", [] {
                                                                return speclab::Test("writeresult-default-refusal-initialized")
                                                                    .Then("setting only the reason leaves the field in its defined default state",
                                                                          [] {
                                                                              speclab::core::Checks checks;
                                                                              mddlog::core::Refusal refusal;
                                                                              refusal.reason = RefusalReason::MalformedTime;
                                                                              checks.expect(refusal.field == IdentifierField::Component,
                                                                                            "identifier field is initialized");
                                                                              checks.raise();
                                                                          })
                                                                    .Execute();
                                                            }};

}  // namespace
