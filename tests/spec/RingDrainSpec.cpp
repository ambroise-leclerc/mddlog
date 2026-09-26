/**
 * @brief Adapter-zone RingLog drain, ownership, field mapping and sink rendering (issue #38).
 */
import std;
import speclab;
import mddlog;
import mddlog.core.ring;

#include "../framework/RecordingSink.hpp"

namespace {

using mddlog::core::Admission;
using mddlog::core::RawTime;
using mddlog::core::RecordInput;
using mddlog::core::RingLog;
using mddlog::spec::RecordingSink;

[[nodiscard]] RecordInput inputWith(std::string_view message,
                                    RawTime          time          = RawTime::unavailable(),
                                    std::string_view component     = "adapter",
                                    std::string_view operationId   = "operation",
                                    std::string_view correlationId = "correlation") noexcept {
    return {.level         = mddlog::LogLevel::Audit,
            .time          = time,
            .location      = std::source_location::current(),
            .message       = message,
            .component     = component,
            .operationId   = operationId,
            .correlationId = correlationId};
}

class RefillOnWriteSink : public RecordingSink {
public:
    explicit RefillOnWriteSink(RingLog<1>& source) : RecordingSink("refill"), ring(source) {}

    void write(const mddlog::LogRecord& record) override {
        if (!attemptedRefill) {
            attemptedRefill = true;
            refillAdmitted  = ring.tryWrite(inputWith("second")).admission() == Admission::Written;
        }
        RecordingSink::write(record);
    }

    [[nodiscard]] bool didRefill() const noexcept {
        return refillAdmitted;
    }

private:
    RingLog<1>& ring;
    bool        attemptedRefill{false};
    bool        refillAdmitted{false};
};

const speclab::Register governedFieldsAndTruncationReachRecordingSink{
    "RingSinkAdapter forwards governed fields, truncation, and host time to RecordingSink",
    "unit",
    [] {
        return speclab::Test("ring-adapter-fields-and-time")
            .Then("an owning adapter record retains source fields and distinguishes unavailable time",
                  [] {
                      speclab::core::Checks   checks;
                      RingLog<2>              ring;
                      mddlog::RingSinkAdapter adapter;
                      auto                    sink = std::make_shared<RecordingSink>();
                      adapter.addRing(ring);
                      adapter.addSink(sink);
                      const std::string longMessage(mddlog::core::messageCapacity + 1, 'x');
                      const auto        supplied = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::milliseconds{123}};
                      const auto        site     = std::source_location::current();
                      const RecordInput input{.level         = mddlog::LogLevel::Audit,
                                              .time          = RawTime::available(supplied),
                                              .location      = site,
                                              .message       = longMessage,
                                              .component     = "pump",
                                              .operationId   = "calibrate",
                                              .correlationId = "cycle-42"};
                      checks.expect(ring.tryWrite(input).admission() == Admission::Written, "governed record is admitted");
                      checks.expect(adapter.drainOnce() == 1, "one record is copied and acknowledged");
                      checks.expect(ring.drain().empty(), "read cursor was advanced");
                      const auto records = sink->records();
                      checks.expect(records.size() == 1, "recording sink receives one owned record");
                      const auto& record = records.front();
                      checks.expect(record.level == mddlog::LogLevel::Audit, "level is retained");
                      checks.expect(record.message == std::string(mddlog::core::messageCapacity, 'x'), "bounded message bytes are retained");
                      checks.expect(record.messageTruncated, "message-truncation flag reaches the sink");
                      checks.expect(record.category == "pump" && record.operationId == "calibrate" && record.correlationId == "cycle-42",
                                    "component, operation, and correlation are retained");
                      checks.expect(record.location.line() == site.line(), "producer source location is retained");
                      checks.expect(record.timeAvailable && record.timestamp == supplied, "host-supplied time is retained without a clock read");
                      checks.expect(record.getFormattedTimestamp().starts_with("1970-01-01T00:00:00"), "available time is rendered in UTC");

                      checks.expect(ring.tryWrite(inputWith("clock unavailable")).admission() == Admission::Written, "unavailable time is admitted");
                      checks.expect(adapter.drainOnce() == 1, "unavailable-time record is drained");
                      const auto withUnavailable = sink->records();
                      checks.expect(!withUnavailable.back().timeAvailable, "unavailable time is not misrepresented as epoch");
                      checks.expect(withUnavailable.back().getFormattedTimestamp() == "unavailable", "unavailable time has an explicit marker");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register copyPrecedesAckAndSlotReuse{"RingSinkAdapter owns a copied record before releasing its ring slot", "unit", [] {
                                                        return speclab::Test("ring-adapter-copy-before-ack")
                                                            .Then("a sink refills a released slot while the original owned record remains unchanged",
                                                                  [] {
                                                                      speclab::core::Checks   checks;
                                                                      RingLog<1>              ring;
                                                                      mddlog::RingSinkAdapter adapter;
                                                                      auto                    sink = std::make_shared<RefillOnWriteSink>(ring);
                                                                      adapter.addRing(ring);
                                                                      adapter.addSink(sink);
                                                                      checks.expect(ring.tryWrite(inputWith("first")).admission() == Admission::Written,
                                                                                    "first slot is published");
                                                                      checks.expect(adapter.drainOnce() == 1, "first record is drained");
                                                                      checks.expect(sink->didRefill(), "slot is reusable when sink receives the copied record");
                                                                      const auto firstBatch = sink->records();
                                                                      checks.expect(firstBatch.size() == 1 && firstBatch.front().message == "first",
                                                                                    "refill did not alter copied first record");
                                                                      checks.expect(adapter.drainOnce() == 1, "second record is drained on the next pass");
                                                                      const auto allRecords = sink->records();
                                                                      checks.expect(allRecords.size() == 2 && allRecords.back().message == "second",
                                                                                    "refilled record is delivered once");
                                                                      checks.expect(ring.drain().empty(), "both records were acknowledged");
                                                                      checks.raise();
                                                                  })
                                                            .Execute();
                                                    }};

const speclab::Register multipleRingsAndRefusalCounts{
    "RingSinkAdapter drains several rings and exposes their separate RingFull counts",
    "unit",
    [] {
        return speclab::Test("ring-adapter-multiple-rings")
            .Then("registered rings are independently acknowledged and counted without a global order claim",
                  [] {
                      speclab::core::Checks   checks;
                      RingLog<2>              first;
                      RingLog<3>              second;
                      mddlog::RingSinkAdapter adapter;
                      auto                    sink = std::make_shared<RecordingSink>();
                      adapter.addRing(first);
                      adapter.addRing(second);
                      adapter.addSink(sink);
                      checks.expect(first.tryWrite(inputWith("first-0")).admission() == Admission::Written, "first ring accepts its first record");
                      checks.expect(first.tryWrite(inputWith("first-1")).admission() == Admission::Written, "first ring accepts its second record");
                      checks.expect(first.tryWrite(inputWith("refused")).admission() == Admission::Refused, "first ring saturates");
                      checks.expect(second.tryWrite(inputWith("second-0")).admission() == Admission::Written, "second ring accepts independently");
                      const auto counts = adapter.refusalCounts();
                      checks.expect(counts.size() == 2 && counts.at(0) == 1 && counts.at(1) == 0, "per-ring saturation counts remain separate");
                      checks.expect(adapter.drainOnce() == 3, "both ring snapshots are copied and acknowledged");
                      checks.expect(first.drain().empty() && second.drain().empty(), "both rings are empty after drain");
                      const auto                 records = sink->records();
                      std::multiset<std::string> messages;
                      for (const auto& record : records)
                          messages.insert(record.message);
                      checks.expect(messages == std::multiset<std::string>{"first-0", "first-1", "second-0"},
                                    "each admitted record reaches the sink exactly once without cross-ring order assumptions");
                      checks.raise();
                  })
            .Execute();
    }};

// NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init): the registry stores a lambda; std::cout is accessed only when the scenario executes.
const speclab::Register consoleMarksTruncatedMessage{"ConsoleSink marks a governed message that was truncated before drain", "unit", [] {
                                                         return speclab::Test("ring-adapter-console-truncation")
                                                             .Then("console output carries an explicit truncation marker",
                                                                   [] {
                                                                       speclab::core::Checks   checks;
                                                                       RingLog<1>              ring;
                                                                       mddlog::RingSinkAdapter adapter;
                                                                       adapter.addRing(ring);
                                                                       adapter.addSink(std::make_shared<mddlog::ConsoleSink>(false, false));
                                                                       const std::string longMessage(mddlog::core::messageCapacity + 1, 'm');
                                                                       checks.expect(ring.tryWrite(inputWith(longMessage)).admission() == Admission::Written,
                                                                                     "long message is admitted");
                                                                       std::ostringstream capture;
                                                                       auto*              original = std::cout.rdbuf(capture.rdbuf());
                                                                       const auto         drained  = adapter.drainOnce();
                                                                       std::cout.rdbuf(original);
                                                                       checks.expect(drained == 1, "message is drained to console");
                                                                       checks.expect(capture.str().find("[truncated]") != std::string::npos,
                                                                                     "console renders truncation explicitly");
                                                                       checks.raise();
                                                                   })
                                                             .Execute();
                                                     }};

}  // namespace
