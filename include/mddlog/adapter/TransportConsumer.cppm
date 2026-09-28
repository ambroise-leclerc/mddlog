/**
 * @brief Bounded, non-reentrant transport consumer with independent health (ADR-003 Decision 5).
 *
 * A transport-backed sink (the motivating case: a WebSocket sink that sends the log line over the
 * same connection it is logging about) must not be driven synchronously from a producer thread:
 * a slow or stalled transport would then block that producer. This adapter consumes from one or
 * more bounded `RingLog`s (ADR-001 Decision 4) on a single host-owned consumer thread, the same
 * pull model `RingSinkAdapter` (RingDrain.cppm) and `AuditSinkAdapter` (AuditDrain.cppm) already
 * use: drainOnce() is called repeatedly by one dedicated thread, never spawned by this adapter.
 *
 * That pull model is what breaks the synchronous re-entrancy cycle (`Frame::addBuffer` logging
 * mid-write) without a separate thread-local guard: a log emitted from inside a transport's
 * write() re-enters the *producer* path (RingLog::tryWrite() on the same ring), not the *consumer*
 * path (SinkRegistry::emit()). The new record lands after the write cursor this drainOnce() call
 * already snapshotted, so it is only visible to the next drainOnce() call - never re-entered
 * within the current one, on any thread.
 *
 * This depends on RingLog's own single-producer contract: a ring written to by a transport's
 * reentrant logging (running on the consumer thread, inside write()) must not also be written to
 * by another, independent producer thread - that would be two producers on one SPSC ring,
 * regardless of this adapter. A host whose transport writes log reentrantly gives the consumer
 * thread its own ring for that purpose, separate from the rings fed by its other producer threads.
 *
 * Detachment on transport failure reuses SinkRegistry's (#67, ADR-003 Decision 4) deferred
 * self-removal: a synchronous throw from write() retires that transport's own handle from inside
 * its own invocation (non-blocking, no further invocation admitted), and an asynchronous failure
 * reported later - from any thread, typically a transport's write-completion handler - retires it
 * through the registry's ordinary removal path, which flips the slot inactive before waiting for
 * any invocation already in flight. Either way the failing transport never receives its own
 * failure record, and health is reported through counters here, never by logging through the sink
 * that just failed.
 */

export module mddlog.adapter.transportconsumer;

import std;
export import mddlog.core.ring;
import mddlog.adapter.logrecord;
import mddlog.adapter.sinkregistry;

export namespace mddlog::adapter {

/** @brief Atomic counters read together are not a single synchronized snapshot. */
struct TransportHealthSnapshot {
    std::uint64_t delivered        = 0;  ///< Successful write() calls
    std::uint64_t writeFailures    = 0;  ///< write() threw synchronously
    std::uint64_t reportedFailures = 0;  ///< reportFailure() called (asynchronous transport error)
    std::uint64_t detachments      = 0;  ///< writeFailures + reportedFailures
    std::uint64_t ringRefusals     = 0;  ///< Aggregate RingFull refusals across registered rings
    std::size_t   activeTransports = 0;  ///< Currently-registered, non-retired transports
};

/**
 * @brief Independent, sink-free health signal for TransportConsumer.
 *
 * Readable without going through any sink - the counters exist precisely because a failing
 * transport must not be handed its own failure line (ADR-003 Decision 5).
 */
class TransportHealth {
public:
    TransportHealth()                                  = default;
    TransportHealth(const TransportHealth&)            = delete;
    TransportHealth& operator=(const TransportHealth&) = delete;
    TransportHealth(TransportHealth&&)                 = delete;
    TransportHealth& operator=(TransportHealth&&)      = delete;
    ~TransportHealth()                                 = default;

    void recordDelivered() noexcept {
        delivered.fetch_add(1, std::memory_order_relaxed);
    }
    void recordWriteFailure() noexcept {
        writeFailures.fetch_add(1, std::memory_order_relaxed);
        detachments.fetch_add(1, std::memory_order_relaxed);
    }
    void recordReportedFailure() noexcept {
        reportedFailures.fetch_add(1, std::memory_order_relaxed);
        detachments.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] TransportHealthSnapshot snapshot() const noexcept {
        return {.delivered        = delivered.load(std::memory_order_relaxed),
                .writeFailures    = writeFailures.load(std::memory_order_relaxed),
                .reportedFailures = reportedFailures.load(std::memory_order_relaxed),
                .detachments      = detachments.load(std::memory_order_relaxed)};
    }

private:
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> writeFailures{0};
    std::atomic<std::uint64_t> reportedFailures{0};
    std::atomic<std::uint64_t> detachments{0};
};

/**
 * @brief Single-consumer bridge from bounded rings to transport-backed write callbacks.
 *
 * Registered rings must outlive this adapter. addRing() mutates this adapter's ring list without
 * synchronization: every addRing() call must complete before the consumer thread starts calling
 * drainOnce() and before any concurrent healthSnapshot() call, exactly as a ring's own producer
 * must be established before that ring is used. Once registration is complete, exactly one
 * consumer thread calls drainOnce(); addTransport()/reportFailure()/healthSnapshot() may then be
 * called from any thread. A transport that throws synchronously, or whose failure is reported
 * later through reportFailure(), is detached before any further record reaches it and receives no
 * invocation after that point.
 *
 * @code
 * TransportConsumer consumer;
 * consumer.addRing(ring);
 * auto handle = consumer.addTransport([](const core::LogRecord& record) { sendOverSocket(record); });
 * // ... on a write-completion failure discovered later, from any thread:
 * consumer.reportFailure(handle);
 * // ... on the one dedicated consumer thread:
 * const auto dispatched = consumer.drainOnce();
 * @endcode
 */
class TransportConsumer {
public:
    using TransportWriteFn = std::function<void(const core::LogRecord&)>;
    using Registry         = SinkRegistry<void(const core::LogRecord&)>;
    using Handle           = Registry::Handle;

    TransportConsumer()                                    = default;
    TransportConsumer(const TransportConsumer&)            = delete;
    TransportConsumer& operator=(const TransportConsumer&) = delete;
    TransportConsumer(TransportConsumer&&)                 = delete;
    TransportConsumer& operator=(TransportConsumer&&)      = delete;
    ~TransportConsumer()                                   = default;

    /**
     * @brief Register a producer-owned ring that must outlive this adapter.
     *
     * Not synchronized against drainOnce() or healthSnapshot(): call this only before the
     * consumer thread starts and before any concurrent observer, as documented on the class.
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
                                    throw std::logic_error("TransportConsumer requires one consumer per ring");
                                return copied;
                            },
                            [&ring] {
                                return ring.refusalCount();
                            }});
    }

    /**
     * @brief Register one transport write callback and return its handle.
     *
     * The wrapped callback records health and, on a synchronous throw, retires its own handle -
     * SinkRegistry recognizes this as self-removal (deferred, non-blocking) because the call
     * happens from inside the very invocation emit() is running.
     *
     * The handle is not known until registry.add() returns, after the closure below is already
     * constructed, so the closure reads it through a small mutex-guarded cell (SelfRef) rather
     * than capturing it directly. addTransport() holds that mutex from before add() until the
     * handle is stored into the cell, so a drainOnce() call that races in and invokes this
     * transport before addTransport() returns blocks on the same mutex until the handle is valid,
     * rather than observing a default-constructed one. registry.add() never invokes any callback
     * (only emit() does), so holding the mutex across it cannot deadlock.
     *
     * The callback captures a weak_ptr to its SelfRef, not a shared_ptr: SelfRef.handle owns a
     * shared_ptr back to this very Slot (that is what makes it useful for self-removal), so a
     * shared_ptr captured inside the Slot's own callback would be a reference cycle no shared_ptr
     * can collect - exactly the leak a cycle like that produces. selfRefs, a member of this
     * adapter rather than of the slot, holds the one strong owner, so the cycle never forms; the
     * callback's weak_ptr only ever resolves while this TransportConsumer itself is alive.
     */
    [[nodiscard]] Handle addTransport(TransportWriteFn write) {
        auto                   self     = std::make_shared<SelfRef>();
        std::weak_ptr<SelfRef> weakSelf = self;
        std::scoped_lock       claim(self->mutex);
        Handle                 handle = registry.add([this, write = std::move(write), weakSelf](const core::LogRecord& record) mutable {
            Handle mine;
            if (auto locked = weakSelf.lock()) {
                std::scoped_lock lock(locked->mutex);
                mine = locked->handle;
            }
            try {
                write(record);
                health.recordDelivered();
            } catch (...) {
                health.recordWriteFailure();
                registry.remove(mine);
            }
        });
        self->handle                  = handle;
        {
            std::scoped_lock lock(selfRefsMutex);
            selfRefs.push_back(std::move(self));
        }
        return handle;
    }

    /**
     * @brief Report an asynchronously-discovered transport failure and detach that transport.
     *
     * Called from any thread, typically a transport's own write-completion handler, after the
     * synchronous invocation that queued the failing operation has already returned - so this is
     * not a self-removal, and SinkRegistry::remove() blocks until quiescent. Its retirement step
     * flips the slot inactive before that wait, so no invocation starting after this call (on any
     * thread, including one racing in concurrently) can reach this transport - "detached before
     * any further emission."
     */
    void reportFailure(const Handle& handle) {
        health.recordReportedFailure();
        registry.remove(handle);
    }

    /**
     * @brief Drain one snapshot from each ring and dispatch every record to every live transport.
     *
     * A record dispatched for an already-retired transport is simply not delivered to it (skipped
     * by the registry's snapshot), so a detachment bounds further write attempts to that
     * transport rather than amplifying them.
     *
     * @return Number of records copied, acknowledged, and dispatched, even if a transport then
     *         fails to write one.
     */
    [[nodiscard]] std::size_t drainOnce() {
        std::size_t dispatched = 0;
        for (const auto& source : ringList) {
            const auto copied = source.drain();
            dispatched       += copied.size();
            for (const auto& record : copied)
                registry.emit(record);
        }
        return dispatched;
    }

    /**
     * @brief Independent health snapshot: readable without going through any sink.
     *
     * Ring refusal counts and the active-transport count are read live from the registered rings
     * and the registry rather than mirrored into TransportHealth, so they always reflect current
     * registration state.
     */
    [[nodiscard]] TransportHealthSnapshot healthSnapshot() const {
        std::uint64_t refusals = 0;
        for (const auto& source : ringList)
            refusals += source.refusalCount();

        auto snapshot             = health.snapshot();
        snapshot.ringRefusals     = refusals;
        snapshot.activeTransports = registry.activeCount();
        return snapshot;
    }

private:
    struct RingSource {
        std::function<std::vector<core::LogRecord>()> drain;
        std::function<std::uint64_t()>                refusalCount;
    };

    /**
     * @brief Lets a registered transport's own callback look up its own Handle.
     *
     * Owned strongly only by TransportConsumer::selfRefs, never by the callback itself (which
     * captures a weak_ptr) - see addTransport()'s own comment for why a shared_ptr there would be
     * a reference cycle back through the very Slot the callback lives in.
     */
    struct SelfRef {
        std::mutex mutex;
        Handle     handle;
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

    std::vector<RingSource>               ringList;
    Registry                              registry;
    TransportHealth                       health;
    std::mutex                            selfRefsMutex;
    std::vector<std::shared_ptr<SelfRef>> selfRefs;
};

}  // namespace mddlog::adapter
