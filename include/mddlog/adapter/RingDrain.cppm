/**
 * @brief Adapter-zone drain from governed SPSC rings to existing sinks (ADR-001 Decision 5).
 */

export module mddlog.adapter.ringdrain;

import std;
export import mddlog.core.ring;
import mddlog.adapter.logrecord;
export import mddlog.sinks.sink;

export namespace mddlog::adapter {

/**
 * @brief Single-consumer bridge from one or more producer-owned rings to existing sinks.
 *
 * Each registered ring has one producer; only one consumer calls drainOnce() or mutates this
 * adapter. Rings must outlive the adapter. Sinks may allocate, format, or perform I/O here, never
 * in mddlog-core. There is no global ordering promise across registered rings.
 *
 * @code
 * RingSinkAdapter adapter;
 * adapter.addRing(ring);
 * adapter.addSink(sink);
 * const auto delivered = adapter.drainOnce();
 * @endcode
 *
 */
class RingSinkAdapter {
public:
    RingSinkAdapter()                                  = default;
    RingSinkAdapter(const RingSinkAdapter&)            = delete;
    RingSinkAdapter& operator=(const RingSinkAdapter&) = delete;
    RingSinkAdapter(RingSinkAdapter&&)                 = delete;
    RingSinkAdapter& operator=(RingSinkAdapter&&)      = delete;
    ~RingSinkAdapter()                                 = default;

    /**
     * @brief Register a producer-owned ring that must outlive this adapter.
     * @tparam Capacity This ring's compile-time capacity; registered rings may differ.
     */
    template <std::size_t Capacity>
    void addRing(core::RingLog<Capacity>& ring) {
        ringList.push_back({[&ring] {
                                const auto                   view = ring.drain();
                                std::vector<core::LogRecord> copied;
                                copied.reserve(view.size());
                                for (const auto& record : view.first())
                                    copied.push_back(copyRecord(record));
                                for (const auto& record : view.second())
                                    copied.push_back(copyRecord(record));

                                // All fields now live in owning adapter records. No span escapes this closure,
                                // and an allocation failure before this point leaves the view unacknowledged.
                                if (!ring.acknowledge(view, view.size()))
                                    throw std::logic_error("RingSinkAdapter requires one consumer per ring");
                                return copied;
                            },
                            [&ring] {
                                return ring.refusalCount();
                            }});
    }

    /** @brief Register a sink for records drained from every ring. Null sinks are ignored. */
    void addSink(sinks::SinkPtr sink) {
        if (sink)
            sinkList.push_back(std::move(sink));
    }

    /**
     * @brief Snapshot RingFull counts in ring registration order.
     *
     * The atomic counts may change while read. This statistic is not a synchronization point and
     * does not provide a consistent cross-ring snapshot. Do not mutate ring registration while
     * reading it from another thread.
     */
    [[nodiscard]] std::vector<std::uint64_t> refusalCounts() const {
        std::vector<std::uint64_t> counts;
        counts.reserve(ringList.size());
        for (const auto& source : ringList)
            counts.push_back(source.refusalCount());
        return counts;
    }

    /**
     * @brief Drain one snapshot from each ring, owning all record data before acknowledgement.
     *
     * Conversion to allocating LogRecord values completes before the read cursor advances. A
     * conversion/allocation failure leaves the snapshot unacknowledged for a later attempt. After
     * acknowledgement, no span is retained: a producer may immediately reuse its slots while
     * sinks process the copied batch. Sink exceptions are isolated and counted as existing sink
     * write failures; losses after admission remain outside this issue's audit-health contract.
     *
     * @return Number of records copied and acknowledged, even if a sink later rejects one.
     */
    [[nodiscard]] std::size_t drainOnce() {
        std::size_t drained = 0;
        for (const auto& source : ringList) {
            const auto copied = source.drain();
            drained          += copied.size();

            for (const auto& record : copied)
                writeToSinks(record);
        }
        return drained;
    }

private:
    struct RingSource {
        std::function<std::vector<core::LogRecord>()> drain;
        std::function<std::uint64_t()>                refusalCount;
    };

    [[nodiscard]] static core::LogRecord copyRecord(const core::GovernedRecord& source) {
        core::LogRecord record;
        record.level            = source.level();
        record.message          = source.message();
        record.messageTruncated = source.truncated().message;
        record.category         = source.component();
        record.location         = source.location();
        record.operationId      = source.operationId();
        record.correlationId    = source.correlationId();
        record.timeAvailable    = source.time().availability() == core::TimeAvailability::Available;
        record.timestamp        = core::LogRecord::TimePoint{};
        if (record.timeAvailable)
            record.timestamp = std::chrono::time_point_cast<core::LogRecord::TimePoint::duration>(source.time().value());
        return record;
    }

    void writeToSinks(const core::LogRecord& record) {
        for (const auto& sink : sinkList) {
            if (!sink->shouldLog(record.level) || !sink->isEnabled())
                continue;
            try {
                sink->write(record);
            } catch (...) {
                sink->recordWriteFailure();
            }
        }
    }

    std::vector<RingSource>     ringList;
    std::vector<sinks::SinkPtr> sinkList;
};

}  // namespace mddlog::adapter
