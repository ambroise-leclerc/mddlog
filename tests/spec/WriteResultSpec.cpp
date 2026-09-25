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

static_assert(std::is_trivially_copyable_v<WriteResult>);
static_assert(noexcept(WriteResult::written()));
static_assert(noexcept(WriteResult::refused({.reason = RefusalReason::RingFull, .field = IdentifierField::Component})));

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
                      checks.expect(whole.admission == Admission::Written, "whole message is admitted");
                      checks.expect(!whole.truncated.message, "whole message is not marked truncated");
                      checks.expect(shortened.admission == Admission::Written, "shortened message is admitted");
                      checks.expect(shortened.truncated.message, "shortened message is marked truncated");
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
                      checks.expect(full.admission == Admission::Refused, "full ring refuses the write");
                      checks.expect(full.refusal.reason == RefusalReason::RingFull, "ring-full reason is preserved");
                      checks.expect(!full.truncated.message, "ring-full refusal has no truncation");

                      constexpr std::array fields{IdentifierField::Component, IdentifierField::OperationId, IdentifierField::CorrelationId};
                      for (const IdentifierField field : fields) {
                          const WriteResult tooLong = WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = field});
                          checks.expect(tooLong.admission == Admission::Refused, "overlong identifier refuses the write");
                          checks.expect(tooLong.refusal.reason == RefusalReason::IdentifierTooLong, "identifier reason is preserved");
                          checks.expect(tooLong.refusal.field == field, "the offending identifier field is preserved");
                          checks.expect(!tooLong.truncated.message, "identifier refusal has no truncation");
                      }

                      constexpr WriteResult malformed = WriteResult::refused({.reason = RefusalReason::MalformedTime, .field = IdentifierField::Component});
                      checks.expect(malformed.admission == Admission::Refused, "malformed time refuses the write");
                      checks.expect(malformed.refusal.reason == RefusalReason::MalformedTime, "malformed-time reason is preserved");
                      checks.expect(!malformed.truncated.message, "malformed-time refusal has no truncation");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
