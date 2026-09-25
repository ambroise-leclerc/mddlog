/**
 * @brief Fixed-capacity single-producer/single-consumer governed record queue (ADR-001 Decision 4).
 */

export module mddlog.core.ring;

import std;
export import mddlog.core.record;

export namespace mddlog::core {

/**
 * @brief Bounded SPSC queue that refuses new writes when all slots are occupied.
 *
 * Exactly one producer calls tryWrite() and exactly one consumer calls drain()/acknowledge() on an
 * instance. Multiple producers use separate rings; no ordering between rings is promised. The
 * producer never overwrites a published record until the consumer releases its slot. All storage
 * is inline and the write path has no allocation, exception, blocking wait or unbounded retry.
 *
 * @code
 * RingLog<8> ring;
 * if (auto result = ring.tryWrite(input); result.admission() == Admission::Refused) {
 *     // Producer thread: handle refusal before continuing.
 * }
 * auto view = ring.drain();            // consumer thread
 * for (const auto& record : view.first()) consume(record);
 * for (const auto& record : view.second()) consume(record);
 * if (!ring.acknowledge(view, view.size())) {
 *     // Stale view: report the error; this call released no slots.
 * }
 * @endcode
 *
 * @tparam Capacity Number of records held at once; must be positive and below half the 64-bit
 *         sequence range, so unsigned cursor subtraction identifies occupancy without ambiguity.
 */
template <std::size_t Capacity>
class RingLog {
    static_assert(Capacity > 0);
    static_assert(Capacity <= std::numeric_limits<std::uint64_t>::max() / 2);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    static constexpr std::size_t capacity = Capacity;

    /**
     * @brief One or two read-only spans over records published at one consumer snapshot.
     *
     * Read or copy records before acknowledging. Every span becomes invalid when any positive
     * number of records from this view is acknowledged, even if the underlying bytes have not
     * yet been reused. Do not retain a span across acknowledgement.
     */
    class DrainView {
    public:
        [[nodiscard]] constexpr std::span<const GovernedRecord> first() const noexcept {
            return firstSpan;
        }

        [[nodiscard]] constexpr std::span<const GovernedRecord> second() const noexcept {
            return secondSpan;
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return firstSpan.size() + secondSpan.size();
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return size() == 0;
        }

    private:
        friend class RingLog;

        constexpr DrainView(const RingLog*                  source,
                            std::uint64_t                   start,
                            std::span<const GovernedRecord> firstPart,
                            std::span<const GovernedRecord> secondPart) noexcept
            : owner(source), readSequence(start), firstSpan(firstPart), secondSpan(secondPart) {}

        const RingLog*                  owner;
        std::uint64_t                   readSequence;
        std::span<const GovernedRecord> firstSpan;
        std::span<const GovernedRecord> secondSpan;
    };

    RingLog() noexcept                 = default;
    RingLog(const RingLog&)            = delete;
    RingLog& operator=(const RingLog&) = delete;
    RingLog(RingLog&&)                 = delete;
    RingLog& operator=(RingLog&&)      = delete;
    ~RingLog()                         = default;

    /**
     * @brief Validate and publish one record, or return the first refusal reason.
     *
     * Identifier checks precede the ring-capacity check in component, operation, correlation
     * order. RingFull is reported last and alone increments the atomic saturation counter. No refusal
     * advances the write cursor or modifies any published record. Unavailable RawTime is valid;
     * MalformedTime cannot arise from the current RawTime representation.
     */
    [[nodiscard]] WriteResult tryWrite(const RecordInput& input) noexcept {
        // Preflight the identifiers before capacity, as ADR-001 requires. assign() checks them
        // again while constructing the record; keeping that guard protects direct record users.
        if (input.component.size() > componentCapacity) {
            return WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::Component});
        }
        if (input.operationId.size() > operationIdCapacity) {
            return WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::OperationId});
        }
        if (input.correlationId.size() > correlationIdCapacity) {
            return WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::CorrelationId});
        }

        const std::uint64_t write = writeCursor.load(std::memory_order_relaxed);  // producer-owned cursor
        // Step 4: acquire the consumer's release of its last reads before reusing any freed slot.
        const std::uint64_t read = readCursor.load(std::memory_order_acquire);
        if (write - read >= Capacity) {
            refusals.fetch_add(1, std::memory_order_relaxed);
            return WriteResult::refused({.reason = RefusalReason::RingFull});
        }

        const auto        index  = static_cast<std::size_t>(write % Capacity);
        const WriteResult result = std::span{slots}[index].assign(input);
        if (result.admission() == Admission::Refused) {
            // Defensive if record validation grows: only RingFull affects saturation telemetry.
            return result;
        }
        // Step 1: fill the slot before publishing the next write sequence with release ordering.
        writeCursor.store(write + 1, std::memory_order_release);
        return result;
    }

    /** @brief Snapshot all currently published unread records for the sole consumer. */
    [[nodiscard]] DrainView drain() noexcept {
        const std::uint64_t read = readCursor.load(std::memory_order_relaxed);  // consumer-owned cursor
        // Step 2: acquire the producer's release publication before reading any slot contents.
        const std::uint64_t                   write      = writeCursor.load(std::memory_order_acquire);
        const auto                            unread     = static_cast<std::size_t>(write - read);
        const auto                            start      = static_cast<std::size_t>(read % Capacity);
        const std::size_t                     firstSize  = std::min(unread, Capacity - start);
        const std::size_t                     secondSize = unread - firstSize;
        const std::span<const GovernedRecord> all{slots};
        return {this, read, all.subspan(start, firstSize), all.first(secondSize)};
    }

    /**
     * @brief Release the copied/read prefix of a drain view for producer reuse.
     * @return false without changing the cursor for a foreign or stale view, or for a count
     *         larger than the view. A positive successful acknowledgement invalidates both spans.
     */
    [[nodiscard]] bool acknowledge(const DrainView& view, std::size_t count) noexcept {
        const std::uint64_t read = readCursor.load(std::memory_order_relaxed);
        if (view.owner != this || view.readSequence != read || count > view.size())
            return false;
        if (count == 0)
            return true;
        // Step 3: after the consumer's last read/copy, release the read cursor. The producer's
        // acquire load in Step 4 completes the second exchange before it overwrites this storage.
        readCursor.store(read + count, std::memory_order_release);
        return true;
    }

    /** @brief Total RingFull refusals, readable concurrently by an observer. Natural 64-bit wrap. */
    [[nodiscard]] std::uint64_t refusalCount() const noexcept {
        return refusals.load(std::memory_order_relaxed);
    }

    /** @brief Snapshot of the producer's published sequence; no cross-cursor snapshot is promised. */
    [[nodiscard]] std::uint64_t writeSequence() const noexcept {
        return writeCursor.load(std::memory_order_acquire);
    }

    /** @brief Snapshot of the consumer's released sequence; no cross-cursor snapshot is promised. */
    [[nodiscard]] std::uint64_t readSequence() const noexcept {
        return readCursor.load(std::memory_order_acquire);
    }

private:
    std::array<GovernedRecord, Capacity> slots{};
    std::atomic<std::uint64_t>           writeCursor{0};
    std::atomic<std::uint64_t>           readCursor{0};
    std::atomic<std::uint64_t>           refusals{0};
};

}  // namespace mddlog::core
