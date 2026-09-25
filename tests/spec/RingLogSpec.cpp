/**
 * @brief Single-threaded bounded SPSC ring contract (issue #36).
 */
import std;
import speclab;
import mddlog.core.ring;

namespace {

using mddlog::core::Admission;
using mddlog::core::IdentifierField;
using mddlog::core::RawTime;
using mddlog::core::RecordInput;
using mddlog::core::RefusalReason;
using mddlog::core::RingLog;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(noexcept(std::declval<RingLog<3>&>().tryWrite(std::declval<const RecordInput&>())));
static_assert(noexcept(std::declval<RingLog<3>&>().drain()));

[[nodiscard]] RecordInput inputWith(std::string_view message, std::string_view component = "core") noexcept {
    return {.time = RawTime::unavailable(), .message = message, .component = component, .operationId = "ring", .correlationId = "test"};
}

const speclab::Register fullRingRefusesWithoutOverwrite{
    "RingLog refuses saturation without overwriting and resumes after acknowledgement",
    "unit",
    [] {
        return speclab::Test("ring-full-and-resume")
            .Then("a full ring retains all published records and accepts another write after a slot is released",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<3>            ring;
                      checks.expect(ring.drain().empty(), "new ring is empty");
                      for (const auto* message : {"A", "B", "C"})
                          checks.expect(ring.tryWrite(inputWith(message)).admission() == Admission::Written, "free slot admits a record");
                      const auto refused = ring.tryWrite(inputWith("D"));
                      checks.expect(refused.admission() == Admission::Refused, "full ring refuses the next write");
                      checks.expect(refused.refusal()->reason == RefusalReason::RingFull, "capacity refusal is RingFull");
                      checks.expect(!refused.truncated().message, "refusal has no truncation");
                      checks.expect(ring.refusalCount() == 1, "one refusal is counted");
                      checks.expect(ring.writeSequence() == 3 && ring.readSequence() == 0, "refusal does not move either cursor");

                      const auto view = ring.drain();
                      checks.expect(view.first().size() == 3 && view.second().empty(), "initial snapshot is one span");
                      checks.expect(view.first()[0].message() == "A" && view.first()[1].message() == "B" && view.first()[2].message() == "C",
                                    "saturation never overwrites accepted records");
                      checks.expect(ring.acknowledge(view, 2), "a prefix can be acknowledged");
                      checks.expect(ring.readSequence() == 2, "read cursor advances by the acknowledged count");
                      checks.expect(ring.tryWrite(inputWith("D")).admission() == Admission::Written, "released slot admits D");
                      checks.expect(ring.tryWrite(inputWith("E")).admission() == Admission::Written, "second released slot admits E");

                      const auto wrapped = ring.drain();
                      checks.expect(wrapped.first().size() == 1 && wrapped.second().size() == 2, "wrapped snapshot has two spans");
                      checks.expect(wrapped.first()[0].message() == "C" && wrapped.second()[0].message() == "D" && wrapped.second()[1].message() == "E",
                                    "records remain in sequence across wrap");
                      checks.expect(ring.acknowledge(wrapped, wrapped.size()), "all remaining records can be acknowledged");
                      checks.expect(ring.drain().empty(), "ring is empty after complete acknowledgement");
                      checks.expect(ring.tryWrite(inputWith("F")).admission() == Admission::Written, "slot reuse resumes after drain");
                      checks.expect(ring.refusalCount() == 1, "successful writes do not increment the refusal counter");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register acknowledgementsAreBoundedAndViewsExpire{
    "RingLog acknowledgement rejects stale, foreign, and excessive requests",
    "unit",
    [] {
        return speclab::Test("ring-acknowledgement-contract")
            .Then("only a prefix of the current ring view may release slots",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<2>            ring;
                      RingLog<2>            other;
                      checks.expect(ring.tryWrite(inputWith("A")).admission() == Admission::Written, "A is written");
                      checks.expect(ring.tryWrite(inputWith("B")).admission() == Admission::Written, "B is written");
                      const auto view = ring.drain();
                      checks.expect(!other.acknowledge(view, 1), "a foreign ring cannot acknowledge this view");
                      checks.expect(!ring.acknowledge(view, 3), "acknowledgement cannot exceed view size");
                      checks.expect(ring.acknowledge(view, 0), "zero acknowledgement is harmless");
                      checks.expect(ring.readSequence() == 0, "invalid and zero acknowledgements retain the cursor");
                      checks.expect(ring.acknowledge(view, 1), "one record is released");
                      checks.expect(!ring.acknowledge(view, 1), "the prior view is stale after positive acknowledgement");
                      checks.expect(ring.readSequence() == 1, "stale acknowledgement cannot double-release");
                      const auto remaining = ring.drain();
                      checks.expect(remaining.size() == 1 && remaining.first()[0].message() == "B", "unreleased record is retained");
                      checks.expect(ring.acknowledge(remaining, 1), "fresh view releases the remainder");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register validationPrecedesCapacityAndTruncationOnlyOnWrite{
    "RingLog validates before capacity and only written messages may be truncated",
    "unit",
    [] {
        return speclab::Test("ring-refusal-order-and-truncation")
            .Then("invalid identifiers win over RingFull and leave all published bytes untouched",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<1>            ring;
                      const std::string     longComponent(mddlog::core::componentCapacity + 1, 'c');
                      const auto            invalidEmpty = ring.tryWrite(inputWith("new", longComponent));
                      checks.expect(invalidEmpty.refusal()->reason == RefusalReason::IdentifierTooLong, "invalid input is refused even when the ring is empty");
                      checks.expect(ring.refusalCount() == 0 && ring.writeSequence() == 0, "identifier refusal does not signal saturation or publish a record");
                      const std::string longMessage(mddlog::core::messageCapacity + 1, 'm');
                      const auto        written = ring.tryWrite(inputWith(longMessage));
                      checks.expect(written.admission() == Admission::Written && written.truncated().message, "overlong message is admitted with truncation");
                      const auto invalid = ring.tryWrite(inputWith("new", longComponent));
                      checks.expect(invalid.refusal()->reason == RefusalReason::IdentifierTooLong, "identifier validation precedes ring saturation");
                      checks.expect(invalid.refusal()->field == IdentifierField::Component, "offending identifier is named");
                      checks.expect(!invalid.truncated().message, "refused write has no truncation");
                      const std::string longOperation(mddlog::core::operationIdCapacity + 1, 'o');
                      RecordInput       invalidOperation = inputWith("new");
                      invalidOperation.operationId       = longOperation;
                      const auto operationResult         = ring.tryWrite(invalidOperation);
                      checks.expect(operationResult.refusal()->reason == RefusalReason::IdentifierTooLong
                                        && operationResult.refusal()->field == IdentifierField::OperationId,
                                    "operation overflow wins over saturation");
                      const std::string longCorrelation(mddlog::core::correlationIdCapacity + 1, 'r');
                      RecordInput       invalidCorrelation = inputWith("new");
                      invalidCorrelation.correlationId     = longCorrelation;
                      const auto correlationResult         = ring.tryWrite(invalidCorrelation);
                      checks.expect(correlationResult.refusal()->reason == RefusalReason::IdentifierTooLong
                                        && correlationResult.refusal()->field == IdentifierField::CorrelationId,
                                    "correlation overflow wins over saturation");
                      const auto full = ring.tryWrite(inputWith("new"));
                      checks.expect(full.refusal()->reason == RefusalReason::RingFull, "valid input sees RingFull");
                      checks.expect(ring.refusalCount() == 1, "only RingFull increments the saturation counter");
                      checks.expect(ring.writeSequence() == 1, "no refusal publishes a record");
                      const auto view = ring.drain();
                      checks.expect(view.size() == 1 && view.first()[0].message() == std::string(mddlog::core::messageCapacity, 'm'),
                                    "refusals retain the prior published record");
                      checks.expect(view.first()[0].truncated().message, "published record retains its truncation flag");
                      checks.expect(ring.acknowledge(view, 1), "published record is released");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register rotationsPreserveEveryRecord{
    "RingLog preserves every accepted record over many slot rotations",
    "unit",
    [] {
        return speclab::Test("ring-many-rotations")
            .Then("alternating writes and partial drains never lose, duplicate, or overwrite records",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<5>            ring;
                      constexpr int         recordCount = 2000;
                      int                   nextWritten = 0;
                      int                   nextRead    = 0;
                      while (nextRead < recordCount) {
                          while (nextWritten < recordCount && nextWritten - nextRead < 5) {
                              const auto message = std::to_string(nextWritten);
                              checks.expect(ring.tryWrite(inputWith(message)).admission() == Admission::Written, "free slot admits next sequence value");
                              ++nextWritten;
                          }
                          const auto  view    = ring.drain();
                          const auto  count   = std::min<std::size_t>(view.size(), 3);
                          std::size_t checked = 0;
                          for (const auto& record : view.first()) {
                              if (checked == count)
                                  break;
                              checks.expect(record.message() == std::to_string(nextRead), "first span preserves sequence");
                              ++checked;
                              ++nextRead;
                          }
                          for (const auto& record : view.second()) {
                              if (checked == count)
                                  break;
                              checks.expect(record.message() == std::to_string(nextRead), "second span preserves sequence");
                              ++checked;
                              ++nextRead;
                          }
                          checks.expect(ring.acknowledge(view, checked), "checked prefix is released");
                      }
                      checks.expect(nextWritten == recordCount && nextRead == recordCount, "all values are written and consumed exactly once");
                      checks.expect(ring.writeSequence() == recordCount && ring.readSequence() == recordCount, "monotonic cursors agree with total records");
                      checks.expect(ring.refusalCount() == 0 && ring.drain().empty(), "no refusal or unread record remains");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
