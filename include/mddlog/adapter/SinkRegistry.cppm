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
            return slot != nullptr;
        }

        friend bool operator==(const Handle&, const Handle&) noexcept = default;

    private:
        friend class SinkRegistry;

        Handle(std::shared_ptr<Slot> registeredSlot, std::uint32_t registeredGeneration, const SinkRegistry* owningRegistry) noexcept
            : slot(std::move(registeredSlot)), generation(registeredGeneration), owner(owningRegistry) {}

        std::shared_ptr<Slot> slot;
        std::uint32_t         generation = 0;
        const SinkRegistry*   owner      = nullptr;
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
     * A stale or already-removed handle is a safe no-op, and so is a handle that was never
     * registered on this registry instance (each Handle remembers which registry issued it).
     */
    void remove(const Handle& handle) {
        if (!handle.valid() || handle.owner != this)
            return;
        Slot& slot = *handle.slot;

        const bool isSelfRemoval = isCurrentlyInvokingOnThisThread(&slot) || isCurrentlyReleasingOnThisThread(&slot);

        if (!retire(slot, handle.generation))
            return;  // already retired/reused under this generation: nothing to do

        if (isSelfRemoval) {
            return;  // deferred: quiescence completes on its own when the current call returns
        }

        const std::thread::id me      = std::this_thread::get_id();
        const auto&           myStack = localInvocationStack();
        {
            std::scoped_lock lock(waitGraphMutex);
            waitingFor[me] = &slot;
            if (!myStack.empty()) {
                // Cross-sink removal: is some thread U currently invoking `slot` (our target)
                // itself blocked waiting for a slot that *we* are currently invoking (anywhere in
                // our own stack, not just the innermost frame - nested emit() can put several
                // slots in flight on this thread at once)? If so, waiting here would complete only
                // once U's wait completes, which only happens once one of our own invocations
                // returns - a two-party cycle. Break it by not blocking; the other side proceeds
                // and this handle still finishes retiring on its own once quiescent.
                for (const auto& [otherThread, invokingSlots] : invoking) {
                    if (std::find(invokingSlots.begin(), invokingSlots.end(), &slot) == invokingSlots.end())
                        continue;
                    const auto waiting = waitingFor.find(otherThread);
                    if (waiting == waitingFor.end())
                        continue;
                    if (std::find(myStack.begin(), myStack.end(), waiting->second) != myStack.end()) {
                        waitingFor.erase(me);
                        return;
                    }
                }
            }
        }

        waitForQuiescence(slot, handle.generation);

        {
            std::scoped_lock lock(waitGraphMutex);
            waitingFor.erase(me);
        }

        finalize(slot, handle.generation);
        // finalize() releases the callback in two locked phases with the destruction itself
        // outside the lock (see its own comment); a thread that loses the claim to do that
        // release returns from finalize() immediately, before the actual winner has necessarily
        // finished. Waiting again here - which is a no-op if this thread *was* the winner, since
        // finalize() only returns once its own two phases are both complete - is what keeps this
        // remove() call from returning while the callback is still being destroyed elsewhere.
        waitForQuiescence(slot, handle.generation);
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
        const auto snapshot = std::atomic_load_explicit(&published, std::memory_order_acquire);
        if (!snapshot)
            return;

        for (const auto& entry : *snapshot) {
            Slot& slot = *entry.slot;
            if (!enter(slot, entry.generation))
                continue;

            auto& stack = localInvocationStack();
            stack.push_back(&slot);
            const std::thread::id me = std::this_thread::get_id();
            {
                std::scoped_lock lock(waitGraphMutex);
                invoking[me].push_back(&slot);
            }

            try {
                std::invoke(slot.callback, args...);
            } catch (...) {  // NOLINT(bugprone-empty-catch): a throwing callback must not stop delivery to the
                             // remaining snapshot entries; this registry has no statistics object of its own
                             // to record the failure into (unlike sinks::Sink::recordWriteFailure()).
            }

            {
                std::scoped_lock lock(waitGraphMutex);
                auto&            mine = invoking[me];
                mine.pop_back();
                if (mine.empty())
                    invoking.erase(me);
            }
            stack.pop_back();

            exit(slot);
        }
    }

    /** @brief Number of callbacks currently registered (a snapshot, may change immediately). */
    [[nodiscard]] std::size_t activeCount() const {
        const auto snapshot = std::atomic_load_explicit(&published, std::memory_order_acquire);
        return snapshot ? snapshot->size() : 0;
    }

    /**
     * @brief Number of distinct slot allocations this registry has ever made.
     *
     * A diagnostic for tests: repeated add()/remove() cycles must recycle slots through the free
     * list rather than growing this without bound.
     */
    [[nodiscard]] std::size_t poolSize() const {
        std::scoped_lock lock(publishMutex);
        return pool.size();
    }

private:
    static constexpr std::uint64_t kActiveBit       = std::uint64_t{1} << 31u;
    static constexpr std::uint64_t kCountMask       = kActiveBit - 1;
    static constexpr unsigned      kGenerationShift = 32u;

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

    /**
     * @brief Slots this thread is currently inside an invocation of, outermost first.
     *
     * Ordinarily has at most one entry, but a callback that itself calls emit() (nested
     * emission) pushes another - remove() must recognize self-removal against every frame on
     * this stack, not only the innermost one, or removing an *ancestor* frame's handle from a
     * nested invocation would be treated as an ordinary cross-removal and deadlock waiting for
     * quiescence on an invocation this same thread cannot make progress on.
     */
    [[nodiscard]] static std::vector<const Slot*>& localInvocationStack() noexcept {
        thread_local std::vector<const Slot*> stack;
        return stack;
    }
    [[nodiscard]] static bool isCurrentlyInvokingOnThisThread(const Slot* slot) noexcept {
        const auto& stack = localInvocationStack();
        return std::find(stack.begin(), stack.end(), slot) != stack.end();
    }

    /**
     * @brief Slots this thread is currently destroying a callback for, inside finalize().
     *
     * A callback's capture can itself be an RAII handle whose destructor calls remove() on the
     * very handle that owns it - not a self-removal from inside the callback's own invocation
     * (localInvocationStack() no longer lists this slot by the time finalize() runs: emit() pops
     * it before calling exit()), but a self-removal from inside the callback's *destruction*.
     * Only this thread, finishing the destruction already in progress, can advance the slot past
     * its releasing sentinel; waiting for that from inside the very call that is blocking it would
     * be a self-deadlock. remove() treats a slot on this stack the same as an ordinary
     * self-removal: return immediately and let the enclosing finalize() complete it.
     */
    [[nodiscard]] static std::vector<const Slot*>& localReleasingStack() noexcept {
        thread_local std::vector<const Slot*> stack;
        return stack;
    }
    [[nodiscard]] static bool isCurrentlyReleasingOnThisThread(const Slot* slot) noexcept {
        const auto& stack = localReleasingStack();
        return std::find(stack.begin(), stack.end(), slot) != stack.end();
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
     * @brief Leave an invocation entered via enter(). Wakes a blocked remove() and attempts
     *        finalize() itself, so the slot is released whether or not anyone is waiting on it
     *        (the self-removal and deferred-cross-removal case have no waiter to do it instead).
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

    /**
     * @brief Block until @p generation's retirement is quiescent - or until it no longer is this
     *        generation at all.
     *
     * The generation check, not just the count, is what makes this correct: once this slot is
     * fully quiescent, finalize() advances its generation and it may immediately be handed to a
     * new add() (the free list is what recycles it, and reuse can happen before this thread ever
     * wakes up). Stopping only on countOf(observed) == 0 would then have this thread keep waiting
     * on - and be woken by - an entirely unrelated *new* registration's own traffic through the
     * same slot, which can run indefinitely and has no reason to ever quiesce on this thread's
     * account. Once generationOf(observed) has moved past @p generation, this generation's count
     * already reached zero (that is the only way finalize() advances it), so there is nothing left
     * to wait for.
     */
    static void waitForQuiescence(Slot& slot, std::uint32_t generation) noexcept {
        auto observed = slot.state.load(std::memory_order_acquire);
        while (generationOf(observed) == generation && countOf(observed) != 0) {
            slot.state.wait(observed, std::memory_order_acquire);
            observed = slot.state.load(std::memory_order_acquire);
        }
    }

    /**
     * @brief Release the callback's resources and return the slot to the free list.
     *
     * Idempotent and safe to call from more than one thread for the same retirement (exit() and a
     * blocked remove() may both reach this for the same slot): only the thread that wins the first
     * CAS below performs the release; the other returns immediately (its caller, remove(), waits
     * again afterwards - see its own comment - rather than relying on this call alone).
     *
     * The release happens in two locked phases with the callback's destruction itself *outside*
     * any lock, in between:
     *
     * 1. Claim the retirement by moving `pack(generation, false, 0)` (quiescent) to a "releasing"
     *    sentinel that keeps the same generation but sets the count to kCountMask. enter() still
     *    rejects it (inactive); retire() still treats it as already retiring; waitForQuiescence()
     *    still waits on it (its count is not 0, so a waiter for this generation is not fooled into
     *    returning early). Move the callback out into a local and publish the snapshot without
     *    this slot, all under publishMutex.
     * 2. Destroy the local, and with it the callback's captures - with no lock held. Captures are
     *    user code: a capture can be an RAII handle whose destructor itself calls add() or
     *    remove() on this same registry, and both of those take publishMutex. Running that
     *    destructor while still holding the lock this function took to get here would be a
     *    same-thread double lock of a non-recursive mutex - undefined behavior, and in practice a
     *    deadlock - exactly the class of bug this registry exists to rule out, just one level
     *    removed (through a capture's destructor rather than a callback body).
     *
     * Only once destruction has completed does a third, brief locked step push the slot onto the
     * free list and advance its generation, which is what lets a concurrently blocked
     * waitForQuiescence() (this retirement's or a later add()'s) proceed.
     */
    void finalize(Slot& slot, std::uint32_t generation) {
        Callback released;
        {
            std::scoped_lock lock(publishMutex);

            auto cur = slot.state.load(std::memory_order_acquire);
            if (generationOf(cur) != generation || activeOf(cur) || countOf(cur) != 0)
                return;  // already finalized under this generation, reused, or not actually quiescent
            if (!slot.state.compare_exchange_strong(cur, pack(generation, false, kCountMask), std::memory_order_acq_rel, std::memory_order_acquire))
                return;  // another thread claimed this retirement first

            // Not `released = std::move(slot.callback)`: a moved-from std::function is left in a
            // "valid but unspecified" state by the standard, not guaranteed empty - and, at least
            // with the standard library this was verified against, is not empty in practice,
            // leaking the old target's lifetime into whatever this slot is reused for next. swap()
            // has no such escape hatch: slot.callback is guaranteed empty afterward.
            released.swap(slot.callback);

            auto current = std::atomic_load_explicit(&published, std::memory_order_acquire);
            auto next    = std::make_shared<Snapshot>();
            if (current) {
                next->reserve(current->size());
                for (const auto& entry : *current)
                    if (entry.slot.get() != &slot)
                        next->push_back(entry);
            }
            std::atomic_store_explicit(&published, std::shared_ptr<const Snapshot>(std::move(next)), std::memory_order_release);
        }

        {
            // A capture's destructor may call remove() on this very handle (e.g. an RAII
            // subscription whose destructor unregisters itself). remove() checks this stack and
            // returns immediately rather than waiting on the releasing sentinel only this thread,
            // already inside that same destructor call, could ever clear.
            auto& releasing = localReleasingStack();
            releasing.push_back(&slot);
            released = Callback{};  // destroys the callback's captures; no registry lock is held here
            releasing.pop_back();
        }

        {
            std::scoped_lock lock(publishMutex);
            for (auto& pooled : pool) {
                if (pooled.get() == &slot) {
                    freeList.push_back(pooled);
                    break;
                }
            }
            slot.state.store(pack(generation + 1, false, 0), std::memory_order_release);
        }
        slot.state.notify_all();
    }

    [[nodiscard]] Handle addOne(Callback callback) {
        std::scoped_lock lock(publishMutex);

        std::shared_ptr<Slot> slot;
        if (!freeList.empty()) {
            slot = freeList.back();
            freeList.pop_back();
        } else {
            slot = std::make_shared<Slot>();
            pool.push_back(slot);
        }

        const auto          previous   = slot->state.load(std::memory_order_relaxed);
        const std::uint32_t generation = generationOf(previous) + 1;
        slot->callback                 = std::move(callback);
        slot->state.store(pack(generation, true, 0), std::memory_order_release);

        auto current = std::atomic_load_explicit(&published, std::memory_order_acquire);
        auto next    = std::make_shared<Snapshot>();
        if (current) {
            next->reserve(current->size() + 1);
            next->insert(next->end(), current->begin(), current->end());
        }
        next->push_back(SnapshotEntry{slot, generation});
        std::atomic_store_explicit(&published, std::shared_ptr<const Snapshot>(std::move(next)), std::memory_order_release);

        return Handle(slot, generation, this);
    }

    // Not std::atomic<std::shared_ptr<T>> (P0718): libc++ 21's std::atomic primary template
    // requires a trivially copyable T and does not yet specialize shared_ptr, so this uses the
    // shared_ptr-specific atomic free functions instead. Every access to `published` - reads
    // included - must go through them; a plain load/store on a shared_ptr is not thread-safe.
    mutable std::mutex                 publishMutex;
    std::shared_ptr<const Snapshot>    published;
    std::vector<std::shared_ptr<Slot>> pool;
    std::vector<std::shared_ptr<Slot>> freeList;

    // Deadlock avoidance for mutual cross-removal (Decision 4): kept on the side, touched only
    // while a callback is being entered/exited and while remove() is deciding whether to block,
    // never on the emit() fast path beyond that bookkeeping. invoking mirrors each thread's
    // localInvocationStack() (nested emit() may hold several slots at once), so a thread U is
    // recorded as "currently invoking `slot`" for as long as `slot` is anywhere on U's stack.
    std::mutex                                                    waitGraphMutex;
    std::unordered_map<std::thread::id, std::vector<const Slot*>> invoking;
    std::unordered_map<std::thread::id, const Slot*>              waitingFor;
};

}  // namespace mddlog::adapter
