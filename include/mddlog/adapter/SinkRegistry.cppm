/**
 * @brief Synchronized callback registry with per-callback handles and quiescent removal.
 *
 * Adapter-zone module (ADR-003 Decision 4): replaces the pattern WebFront's `Sinks` class uses
 * today - `inline static std::vector<std::function<...>> sinks` with no synchronization - with a
 * registry that gives every registered callback a stable handle, takes a stable snapshot at
 * emission so registration never races with an in-flight iteration, and lets removal wait until
 * every already-engaged invocation of that one callback has returned. This module is generic
 * (templated on the callback signature) so it is not WebFront-specific; issue #67 builds and
 * tests it standalone, ahead of any consumer.
 */

export module mddlog.adapter.sinkregistry;

import std;

export namespace mddlog::adapter {

/**
 * @brief A synchronized registry of callbacks, addressed by handle, with quiescent removal.
 *
 * @tparam Signature Callback signature, e.g. `void(std::string_view)`.
 *
 * Registration and emission do not race: `emit()` takes an immutable snapshot of the currently
 * live callbacks (an atomically-published `shared_ptr` to a vector of stable, individually
 * heap-allocated slots) and iterates that snapshot only, so a concurrent `add()` never reallocates
 * anything an in-flight `emit()` is walking. Removed slots are recycled through a free list rather
 * than leaked, so long-running add/remove churn does not grow the registry without bound.
 *
 * `remove()` waits for every invocation of that callback already under way to finish before
 * returning - the property a `sinks[id] = nullptr`-style removal does not provide - with two
 * documented exceptions, both required to avoid deadlock:
 *
 * - **Self-removal**: a callback that removes its own handle from inside its own invocation
 *   returns immediately (it cannot wait for itself to finish). No further invocation of that
 *   handle starts; the removal completes, and the callback's resources are released, once the
 *   current invocation returns.
 * - **Mutual cross-removal**: if callback A's invocation is removing B while B's invocation is
 *   concurrently removing A, waiting for full quiescence on both sides would deadlock. This
 *   registry detects exactly that two-party cycle and lets the second remover proceed without
 *   blocking, the same way self-removal does, rather than let both threads wait on each other
 *   forever. A removal issued from inside a *different* callback's invocation that is not part of
 *   such a cycle still blocks for full quiescence, as documented above.
 */
template <typename Signature>
class SinkRegistry;

template <typename Result, typename... Args>
class SinkRegistry<Result(Args...)> {
private:
    struct Slot;  // defined below; forward-declared here so Handle can hold a shared_ptr<Slot>

public:
    using Callback = std::function<Result(Args...)>;

    SinkRegistry()                               = default;
    SinkRegistry(const SinkRegistry&)            = delete;
    SinkRegistry& operator=(const SinkRegistry&) = delete;
    SinkRegistry(SinkRegistry&&)                 = delete;
    SinkRegistry& operator=(SinkRegistry&&)      = delete;
    ~SinkRegistry()                              = default;

    /**
     * @brief One registered callback, addressed by a stable handle.
     *
     * A handle keeps its slot's storage alive (so the handle itself is always safe to hold and
     * to pass to remove(), even long after removal) but the slot's *generation* is what identifies
     * this particular registration: once removed, the slot may be recycled for a later add(), at
     * which point a stale handle's generation no longer matches and any operation on it is a
     * harmless no-op.
     */
    class Handle {
    public:
        Handle() noexcept = default;

        [[nodiscard]] bool valid() const noexcept {
            return slot_ != nullptr;
        }

        friend bool operator==(const Handle&, const Handle&) noexcept = default;

    private:
        friend class SinkRegistry;

        Handle(std::shared_ptr<Slot> slot, std::uint32_t generation) noexcept : slot_(std::move(slot)), generation_(generation) { }

        std::shared_ptr<Slot> slot_;
        std::uint32_t         generation_ = 0;
    };

    /** @brief Register one callback. Single registration keeps this shape: one handle back. */
    [[nodiscard]] Handle add(Callback callback) {
        return addOne(std::move(callback));
    }

    /**
     * @brief Register several callbacks at once, returning one handle per callback in argument
     *        order - fixing the "returns only the last id" defect a fold-and-push_back has.
     *
     * This is a breaking change relative to a single combined identifier: a call site that used
     * to bind one id for several registrations must now bind one handle per callback and pass
     * each individually to remove(). There is nothing to migrate for a call site that already
     * registers a single callback at a time - see add(Callback) above.
     */
    template <typename... Callbacks>
        requires(sizeof...(Callbacks) >= 2)
    [[nodiscard]] std::array<Handle, sizeof...(Callbacks)> add(Callbacks&&... callbacks) {
        return {addOne(Callback(std::forward<Callbacks>(callbacks)))...};
    }

    /**
     * @brief Remove a registered callback, per the class-level contract.
     *
     * Called from ordinary (non-callback) code, or from inside a *different* callback's
     * invocation that is not part of a mutual cross-removal cycle, this blocks until every
     * already-engaged invocation of @p handle has returned. Called from inside the callback
     * being removed (self-removal) or as the losing side of a two-party mutual cross-removal, it
     * returns immediately instead, per the class documentation above.
     *
     * A stale or already-removed handle is a safe no-op.
     */
    void remove(const Handle& handle) {
        if (!handle.valid())
            return;
        Slot& slot = *handle.slot_;

        const auto selfSlot      = threadLocalInvokingSlot();
        const bool isSelfRemoval = selfSlot == &slot;

        if (!retire(slot, handle.generation_))
            return;  // already retired/reused under this generation: nothing to do

        if (isSelfRemoval) {
            return;  // deferred: quiescence completes on its own when the current call returns
        }

        const std::thread::id me = std::this_thread::get_id();
        {
            std::scoped_lock lock(waitGraphMutex_);
            waitingFor_[me] = &slot;
            if (selfSlot != nullptr) {
                // Cross-sink removal: does some thread currently invoking `slot` wait for the
                // slot *we* are being invoked from? If so, waiting here would complete only once
                // that thread's wait completes, which only happens once our own invocation
                // returns - a two-party cycle. Break it by not blocking; the other side proceeds
                // and this handle still finishes retiring on its own once quiescent.
                for (const auto& [otherThread, invokingSlot] : invoking_) {
                    if (invokingSlot != &slot)
                        continue;
                    auto waiting = waitingFor_.find(otherThread);
                    if (waiting != waitingFor_.end() && waiting->second == selfSlot) {
                        waitingFor_.erase(me);
                        return;
                    }
                }
            }
        }

        waitForQuiescence(slot);

        {
            std::scoped_lock lock(waitGraphMutex_);
            waitingFor_.erase(me);
        }

        finalize(slot, handle.generation_);
    }

    /**
     * @brief Invoke every currently-registered callback with @p args, in registration order.
     *
     * Takes one immutable snapshot of the currently live callbacks and iterates it; a callback
     * added or removed while this runs is not guaranteed to be included, but the snapshot itself
     * is never reallocated or corrupted by a concurrent add()/remove(). An exception thrown by one
     * callback is swallowed so the rest of the snapshot is still delivered.
     */
    void emit(Args... args) {
        const auto snapshot = std::atomic_load_explicit(&published_, std::memory_order_acquire);
        if (!snapshot)
            return;

        for (const auto& entry : *snapshot) {
            Slot& slot = *entry.slot;
            if (!enter(slot, entry.generation))
                continue;

            const Slot* previous = threadLocalInvokingSlot();
            setThreadLocalInvokingSlot(&slot);
            {
                std::scoped_lock lock(waitGraphMutex_);
                invoking_[std::this_thread::get_id()] = &slot;
            }

            try {
                std::invoke(slot.callback, args...);
            } catch (...) {
                // A throwing callback must not stop delivery to the remaining snapshot entries.
            }

            {
                std::scoped_lock lock(waitGraphMutex_);
                invoking_.erase(std::this_thread::get_id());
            }
            setThreadLocalInvokingSlot(previous);

            exit(slot);
        }
    }

    /** @brief Number of callbacks currently registered (a snapshot, may change immediately). */
    [[nodiscard]] std::size_t activeCount() const {
        const auto snapshot = std::atomic_load_explicit(&published_, std::memory_order_acquire);
        return snapshot ? snapshot->size() : 0;
    }

    /**
     * @brief Number of distinct slot allocations this registry has ever made.
     *
     * A diagnostic for tests: repeated add()/remove() cycles must recycle slots through the free
     * list rather than growing this without bound.
     */
    [[nodiscard]] std::size_t poolSize() const {
        std::scoped_lock lock(publishMutex_);
        return pool_.size();
    }

private:
    static constexpr std::uint64_t kActiveBit       = std::uint64_t{1} << 31;
    static constexpr std::uint64_t kCountMask       = kActiveBit - 1;
    static constexpr int           kGenerationShift = 32;

    struct Slot {
        std::atomic<std::uint64_t> state{0};  // packed: generation(32) | active(1) | count(31)
        Callback                   callback;
    };

    struct SnapshotEntry {
        std::shared_ptr<Slot> slot;
        std::uint32_t         generation;
    };
    using Snapshot = std::vector<SnapshotEntry>;

    [[nodiscard]] static std::uint32_t generationOf(std::uint64_t s) noexcept {
        return static_cast<std::uint32_t>(s >> kGenerationShift);
    }
    [[nodiscard]] static bool activeOf(std::uint64_t s) noexcept {
        return (s & kActiveBit) != 0;
    }
    [[nodiscard]] static std::uint32_t countOf(std::uint64_t s) noexcept {
        return static_cast<std::uint32_t>(s & kCountMask);
    }
    [[nodiscard]] static std::uint64_t pack(std::uint32_t generation, bool active, std::uint32_t count) noexcept {
        return (std::uint64_t{generation} << kGenerationShift) | (active ? kActiveBit : 0) | (count & kCountMask);
    }

    /** @brief Thread-local slot this thread's callback invocation belongs to, if any. */
    [[nodiscard]] static const Slot*& threadLocalInvokingSlotRef() noexcept {
        thread_local const Slot* current = nullptr;
        return current;
    }
    [[nodiscard]] static const Slot* threadLocalInvokingSlot() noexcept {
        return threadLocalInvokingSlotRef();
    }
    static void setThreadLocalInvokingSlot(const Slot* slot) noexcept {
        threadLocalInvokingSlotRef() = slot;
    }

    /** @brief Try to enter an invocation of @p slot at @p generation. False: skip, do not call. */
    [[nodiscard]] static bool enter(Slot& slot, std::uint32_t generation) noexcept {
        auto cur = slot.state.load(std::memory_order_acquire);
        while (true) {
            if (generationOf(cur) != generation || !activeOf(cur))
                return false;
            const auto next = pack(generation, true, countOf(cur) + 1);
            if (slot.state.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    /**
     * @brief Leave an invocation entered via enter(). Wakes a waiting remove(), and finalizes the
     *        slot itself if no one is waiting (the self-removal and deferred-cross-removal case).
     */
    void exit(Slot& slot) {
        auto cur = slot.state.load(std::memory_order_acquire);
        while (true) {
            const auto next = pack(generationOf(cur), activeOf(cur), countOf(cur) - 1);
            if (slot.state.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (!activeOf(next) && countOf(next) == 0) {
                    slot.state.notify_all();
                    finalize(slot, generationOf(next));
                }
                return;
            }
        }
    }

    /**
     * @brief Clear the active bit for @p generation, if it is still current. Idempotent.
     * @return False if @p generation is stale (already retired and possibly reused): a no-op.
     */
    [[nodiscard]] static bool retire(Slot& slot, std::uint32_t generation) noexcept {
        auto cur = slot.state.load(std::memory_order_acquire);
        while (true) {
            if (generationOf(cur) != generation)
                return false;
            if (!activeOf(cur))
                return true;  // already retiring/retired under this generation
            const auto next = pack(generation, false, countOf(cur));
            if (slot.state.compare_exchange_weak(cur, next, std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
    }

    static void waitForQuiescence(Slot& slot) noexcept {
        auto observed = slot.state.load(std::memory_order_acquire);
        while (countOf(observed) != 0) {
            slot.state.wait(observed, std::memory_order_acquire);
            observed = slot.state.load(std::memory_order_acquire);
        }
    }

    /**
     * @brief Release the callback's resources and return the slot to the free list.
     *
     * Idempotent and safe to call from more than one thread for the same retirement (exit() and a
     * blocked remove() may both reach this for the same slot): only the thread that wins the
     * generation-advancing CAS below performs the work, so the slot is never pushed to the free
     * list twice.
     */
    void finalize(Slot& slot, std::uint32_t generation) {
        auto cur = slot.state.load(std::memory_order_acquire);
        if (generationOf(cur) != generation || activeOf(cur) || countOf(cur) != 0)
            return;  // already finalized under this generation, reused, or not actually quiescent
        if (!slot.state.compare_exchange_strong(cur, pack(generation + 1, false, 0), std::memory_order_acq_rel, std::memory_order_acquire))
            return;  // another thread claimed this retirement first

        std::scoped_lock lock(publishMutex_);
        slot.callback = Callback{};

        auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
        auto next    = std::make_shared<Snapshot>();
        if (current) {
            next->reserve(current->size());
            for (const auto& entry : *current)
                if (entry.slot.get() != &slot)
                    next->push_back(entry);
        }
        std::atomic_store_explicit(&published_, std::shared_ptr<const Snapshot>(std::move(next)), std::memory_order_release);

        for (auto& pooled : pool_) {
            if (pooled.get() == &slot) {
                free_.push_back(pooled);
                break;
            }
        }
    }

    [[nodiscard]] Handle addOne(Callback callback) {
        std::scoped_lock lock(publishMutex_);

        std::shared_ptr<Slot> slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
        } else {
            slot = std::make_shared<Slot>();
            pool_.push_back(slot);
        }

        const auto          previous   = slot->state.load(std::memory_order_relaxed);
        const std::uint32_t generation = generationOf(previous) + 1;
        slot->callback                 = std::move(callback);
        slot->state.store(pack(generation, true, 0), std::memory_order_release);

        auto current = std::atomic_load_explicit(&published_, std::memory_order_acquire);
        auto next    = std::make_shared<Snapshot>();
        if (current) {
            next->reserve(current->size() + 1);
            next->insert(next->end(), current->begin(), current->end());
        }
        next->push_back(SnapshotEntry{slot, generation});
        std::atomic_store_explicit(&published_, std::shared_ptr<const Snapshot>(std::move(next)), std::memory_order_release);

        return Handle(slot, generation);
    }

    // Not std::atomic<std::shared_ptr<T>> (P0718): libc++ 21's std::atomic primary template
    // requires a trivially copyable T and does not yet specialize shared_ptr, so this uses the
    // shared_ptr-specific atomic free functions instead. Every access to `published_` - reads
    // included - must go through them; a plain load/store on a shared_ptr is not thread-safe.
    mutable std::mutex                 publishMutex_;
    std::shared_ptr<const Snapshot>    published_;
    std::vector<std::shared_ptr<Slot>> pool_;
    std::vector<std::shared_ptr<Slot>> free_;

    // Deadlock avoidance for mutual cross-removal (Decision 4): kept on the side, touched only
    // while a callback is being entered/exited and while remove() is deciding whether to block,
    // never on the emit() fast path beyond that bookkeeping.
    std::mutex                                       waitGraphMutex_;
    std::unordered_map<std::thread::id, const Slot*> invoking_;
    std::unordered_map<std::thread::id, const Slot*> waitingFor_;
};

}  // namespace mddlog::adapter
