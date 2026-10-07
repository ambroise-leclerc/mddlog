/** @brief Audit-only ring consumer and independent delivery-health signal. */

export module mddlog.adapter.auditdrain;

import std;
export import mddlog.core.auditring;
export import mddlog.sinks.auditsink;

export namespace mddlog::adapter {

/** @brief Most recent observable audit delivery problem. */
enum class AuditDrainStatus : std::uint8_t {
    Completed,
    MissingSink,
    DisabledSink,
    SinkRejected,
    SinkThrew,
    AcknowledgementFailed,
    InvalidStream,
    DuplicateStream
};

/** @brief Whether addRing() registered a ring, or why it refused it. */
enum class AuditRingRegistration : std::uint8_t { Registered, InvalidStream, DuplicateStream };

struct AuditDrainResult {
    std::size_t      handedOff = 0;
    AuditDrainStatus status    = AuditDrainStatus::Completed;
    std::size_t      attempted = 0;
};

/** @brief Atomic counters; values read together are not a single synchronized snapshot. */
struct AuditHealthSnapshot {
    std::uint64_t    handedOff           = 0;
    std::uint64_t    dispatchFailures    = 0;
    std::uint64_t    configurationErrors = 0;
    std::uint64_t    reportedLosses      = 0;
    std::uint64_t    takenUnacknowledged = 0;
    std::uint64_t    pendingInRings      = 0;
    AuditDrainStatus lastIssue           = AuditDrainStatus::Completed;
    std::uint64_t    admitted            = 0;
    std::uint64_t    ringFullRefusals    = 0;
};

/**
 * @brief Dedicated thread-safe signal for audit delivery after admission.
 *
 * Sinks or hosts must call reportLoss() when they discover a loss after hand-off. A sink that
 * merely rejected an event has not lost it: the event remains in the ring for retry. Neither
 * this signal nor the diagnostic logger can detect an unreported sink-side loss.
 */
class AuditHealth {
public:
    AuditHealth()                              = default;
    AuditHealth(const AuditHealth&)            = delete;
    AuditHealth& operator=(const AuditHealth&) = delete;
    AuditHealth(AuditHealth&&)                 = delete;
    AuditHealth& operator=(AuditHealth&&)      = delete;
    ~AuditHealth()                             = default;

    void reportLoss(std::uint64_t count = 1) noexcept {
        reportedLosses.fetch_add(count, std::memory_order_relaxed);
    }

    [[nodiscard]] AuditHealthSnapshot snapshot(std::uint64_t pendingInRings = 0) const noexcept {
        return {.handedOff           = handedOff.load(std::memory_order_relaxed),
                .dispatchFailures    = dispatchFailures.load(std::memory_order_relaxed),
                .configurationErrors = configurationErrors.load(std::memory_order_relaxed),
                .reportedLosses      = reportedLosses.load(std::memory_order_relaxed),
                .takenUnacknowledged = takenUnacknowledged.load(std::memory_order_relaxed),
                .pendingInRings      = pendingInRings,
                .lastIssue           = lastIssue.load(std::memory_order_relaxed)};
    }

    void recordConfigurationError(AuditDrainStatus issue) noexcept {
        configurationErrors.fetch_add(1, std::memory_order_relaxed);
        lastIssue.store(issue, std::memory_order_relaxed);
    }
    void recordDispatchFailure(AuditDrainStatus issue) noexcept {
        dispatchFailures.fetch_add(1, std::memory_order_relaxed);
        lastIssue.store(issue, std::memory_order_relaxed);
    }
    void recordTakenUnacknowledged() noexcept {
        takenUnacknowledged.fetch_add(1, std::memory_order_relaxed);
    }
    void clearTakenUnacknowledged() noexcept {
        takenUnacknowledged.fetch_sub(1, std::memory_order_relaxed);
    }
    void recordHandOff() noexcept {
        handedOff.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t>    handedOff{0};
    std::atomic<std::uint64_t>    dispatchFailures{0};
    std::atomic<std::uint64_t>    configurationErrors{0};
    std::atomic<std::uint64_t>    reportedLosses{0};
    std::atomic<std::uint64_t>    takenUnacknowledged{0};
    std::atomic<AuditDrainStatus> lastIssue{AuditDrainStatus::Completed};
};

/**
 * @brief Single-consumer bridge from audit rings to one audit-only sink.
 *
 * Registered rings must outlive the adapter: both drainOnce() and healthSnapshot() access them.
 * The adapter owns a shared reference to the sink. Registration, sink replacement and
 * drainOnce() belong to one consumer thread. Complete registration before concurrent observers
 * call healthSnapshot(); producers may write to their own rings concurrently. A successful sink accept() followed by ring
 * acknowledgement is a hand-off, not durable confirmation. A rejection or exception retains
 * the event in its ring. The next drainOnce() retries it and may duplicate a sink side effect
 * if accept() failed after partially acting; sinks should deduplicate by (streamId, sequence).
 *
 * (streamId, sequence) identifies an event only if no two rings aggregated here share a stream
 * identity (ADR-002 Decision 5). addRing() therefore refuses a ring whose identity is invalid or
 * was already registered on this adapter, including a recreated producer that kept its identity
 * while its sequence restarted. Uniqueness across adapters, processes and boot sessions remains
 * the host's responsibility: no string comparison here can establish it.
 */
class AuditSinkAdapter {
public:
    // User-provided rather than defaulted: GCC 16.1 crashed (ICE in synthesize_method) while
    // synthesizing the defaulted constructor in an importer (tests/spec/AuditPolicySpec.cpp).
    // A body in the module interface is not implicitly inline, so it is compiled here once and
    // importers only call it.
    // NOLINTBEGIN(modernize-use-equals-default,hicpp-use-equals-default): GCC 16.1 workaround above.
    AuditSinkAdapter() {}
    ~AuditSinkAdapter() {}
    // NOLINTEND(modernize-use-equals-default,hicpp-use-equals-default)
    AuditSinkAdapter(const AuditSinkAdapter&)            = delete;
    AuditSinkAdapter& operator=(const AuditSinkAdapter&) = delete;
    AuditSinkAdapter(AuditSinkAdapter&&)                 = delete;
    AuditSinkAdapter& operator=(AuditSinkAdapter&&)      = delete;

    /**
     * @brief Register a ring, or refuse it as an observable configuration error.
     *
     * Refused rings are not drained. Registered identities stay reserved for the adapter's
     * lifetime, because rings cannot be removed and a reused identity would make
     * (streamId, sequence) ambiguous.
     */
    template <std::size_t Capacity>
    [[nodiscard]] AuditRingRegistration addRing(core::AuditRing<Capacity>& ring) {
        if (!ring.hasValidIdentity()) {
            health.recordConfigurationError(AuditDrainStatus::InvalidStream);
            return AuditRingRegistration::InvalidStream;
        }
        if (std::ranges::find(streamIds, ring.identity()) != streamIds.end()) {
            health.recordConfigurationError(AuditDrainStatus::DuplicateStream);
            return AuditRingRegistration::DuplicateStream;
        }
        streamIds.emplace_back(ring.identity());
        try {
            addSource(ring);
        } catch (...) {
            // An unregistered ring must not keep its identity reserved, or a retry is refused.
            streamIds.pop_back();
            throw;
        }
        return AuditRingRegistration::Registered;
    }

    /** @brief Replace the audit sink; null means an observable missing-sink configuration. */
    void setSink(sinks::AuditSinkPtr replacement) noexcept {
        sink = std::move(replacement);
    }

private:
    template <std::size_t Capacity>
    void addSource(core::AuditRing<Capacity>& ring) {
        ringList.push_back({[&ring, pendingFailure = false](sinks::AuditSink& auditSink, AuditHealth& signal, std::size_t recordBudget) mutable {
                                AuditDrainResult  result;
                                const std::size_t available = std::min(ring.drain().size(), recordBudget);
                                for (std::size_t index = 0; index < available; ++index) {
                                    if (!auditSink.isEnabled()) {
                                        signal.recordConfigurationError(AuditDrainStatus::DisabledSink);
                                        result.status = AuditDrainStatus::DisabledSink;
                                        break;
                                    }
                                    const auto view = ring.drain();
                                    if (view.empty())
                                        break;
                                    const core::AuditEvent& event = view.first().empty() ? view.second().front() : view.first().front();
                                    ++result.attempted;
                                    bool accepted = false;
                                    try {
                                        accepted = auditSink.accept(event);
                                    } catch (...) {
                                        signal.recordDispatchFailure(AuditDrainStatus::SinkThrew);
                                        result.status = AuditDrainStatus::SinkThrew;
                                        if (!pendingFailure) {
                                            signal.recordTakenUnacknowledged();
                                            pendingFailure = true;
                                        }
                                        break;
                                    }
                                    if (!accepted) {
                                        signal.recordDispatchFailure(AuditDrainStatus::SinkRejected);
                                        result.status = AuditDrainStatus::SinkRejected;
                                        if (!pendingFailure) {
                                            signal.recordTakenUnacknowledged();
                                            pendingFailure = true;
                                        }
                                        break;
                                    }
                                    if (!ring.acknowledge(view, 1)) {
                                        signal.recordDispatchFailure(AuditDrainStatus::AcknowledgementFailed);
                                        result.status = AuditDrainStatus::AcknowledgementFailed;
                                        if (!pendingFailure) {
                                            signal.recordTakenUnacknowledged();
                                            pendingFailure = true;
                                        }
                                        break;
                                    }
                                    if (pendingFailure) {
                                        signal.clearTakenUnacknowledged();
                                        pendingFailure = false;
                                    }
                                    signal.recordHandOff();
                                    ++result.handedOff;
                                }
                                return result;
                            },
                            [&ring] {
                                const auto read  = ring.acknowledgedCount();
                                const auto write = ring.admittedCount();
                                return write >= read ? write - read : std::uint64_t{0};
                            },
                            [&ring] {
                                return ring.admittedCount();
                            },
                            [&ring] {
                                return ring.refusalCount();
                            }});
    }

public:
    /** @brief Attempt one bounded snapshot per ring and acknowledge only accepted events. */
    [[nodiscard]] AuditDrainResult drainOnce(std::size_t maxRecordsPerRing = std::numeric_limits<std::size_t>::max(),
                                             std::size_t maxAttempts       = std::numeric_limits<std::size_t>::max()) {
        if (!sink) {
            health.recordConfigurationError(AuditDrainStatus::MissingSink);
            return {.status = AuditDrainStatus::MissingSink};
        }
        if (!sink->isEnabled()) {
            health.recordConfigurationError(AuditDrainStatus::DisabledSink);
            return {.status = AuditDrainStatus::DisabledSink};
        }
        AuditDrainResult total;
        for (std::size_t visited = 0; visited < ringList.size() && total.attempted < maxAttempts; ++visited) {
            auto& source     = ringList[nextRing];
            nextRing         = (nextRing + 1) % ringList.size();
            const auto one   = source.drain(*sink, health, std::min(maxRecordsPerRing, maxAttempts - total.attempted));
            total.attempted += one.attempted;
            total.handedOff += one.handedOff;
            if (total.status == AuditDrainStatus::Completed)
                total.status = one.status;
        }
        return total;
    }

    [[nodiscard]] AuditHealthSnapshot healthSnapshot() const {
        std::uint64_t pending = 0;
        for (const auto& source : ringList)
            pending += source.pending();
        auto result = health.snapshot(pending);
        for (const auto& source : ringList) {
            result.admitted         += source.admitted();
            result.ringFullRefusals += source.refusals();
        }
        return result;
    }

    /** @brief Independent channel for a sink or host to report a discovered post-hand-off loss. */
    void reportLoss(std::uint64_t count = 1) noexcept {
        health.reportLoss(count);
    }

private:
    struct RingSource {
        std::function<AuditDrainResult(sinks::AuditSink&, AuditHealth&, std::size_t)> drain;
        std::function<std::uint64_t()>                                                pending;
        std::function<std::uint64_t()>                                                admitted;
        std::function<std::uint64_t()>                                                refusals;
    };

    std::size_t              nextRing = 0;
    std::vector<RingSource>  ringList;
    std::vector<std::string> streamIds;
    sinks::AuditSinkPtr      sink;
    AuditHealth              health;
};

}  // namespace mddlog::adapter
