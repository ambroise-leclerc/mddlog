/** @brief Audit-only ring consumer and independent delivery-health signal. */

export module mddlog.adapter.auditdrain;

import std;
export import mddlog.core.auditring;
export import mddlog.sinks.auditsink;

export namespace mddlog::adapter {

/** @brief Most recent observable audit delivery problem. */
enum class AuditDrainStatus : std::uint8_t { Completed, MissingSink, DisabledSink, SinkRejected, SinkThrew, AcknowledgementFailed };

struct AuditDrainResult {
    std::size_t      handedOff = 0;
    AuditDrainStatus status    = AuditDrainStatus::Completed;
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
 * Rings and the sink must outlive any call to drainOnce(). Registration, sink replacement and
 * drainOnce() belong to one consumer thread. Complete registration before concurrent observers
 * call healthSnapshot(); producers may write to their own rings concurrently. A successful sink accept() followed by ring
 * acknowledgement is a hand-off, not durable confirmation. A rejection or exception retains
 * the event in its ring. The next drainOnce() retries it and may duplicate a sink side effect
 * if accept() failed after partially acting; sinks should deduplicate by (streamId, sequence).
 */
class AuditSinkAdapter {
public:
    AuditSinkAdapter()                                   = default;
    AuditSinkAdapter(const AuditSinkAdapter&)            = delete;
    AuditSinkAdapter& operator=(const AuditSinkAdapter&) = delete;
    AuditSinkAdapter(AuditSinkAdapter&&)                 = delete;
    AuditSinkAdapter& operator=(AuditSinkAdapter&&)      = delete;
    ~AuditSinkAdapter()                                  = default;

    template <std::size_t Capacity>
    void addRing(core::AuditRing<Capacity>& ring) {
        ringList.push_back({[&ring, pendingFailure = false](sinks::AuditSink& auditSink, AuditHealth& signal) mutable {
                                AuditDrainResult  result;
                                const std::size_t available = ring.drain().size();
                                for (std::size_t index = 0; index < available; ++index) {
                                    if (!auditSink.isEnabled()) {
                                        signal.recordConfigurationError(AuditDrainStatus::DisabledSink);
                                        result.status = AuditDrainStatus::DisabledSink;
                                        break;
                                    }
                                    const auto view = ring.drain();
                                    if (view.empty())
                                        break;
                                    const core::AuditEvent& event    = view.first().empty() ? view.second().front() : view.first().front();
                                    bool                    accepted = false;
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
                                const auto read = ring.acknowledgedCount();
                                return ring.admittedCount() - read;
                            }});
    }

    /** @brief Replace the audit sink; null means an observable missing-sink configuration. */
    void setSink(sinks::AuditSinkPtr replacement) noexcept {
        sink = std::move(replacement);
    }

    /** @brief Attempt one bounded snapshot per ring and acknowledge only accepted events. */
    [[nodiscard]] AuditDrainResult drainOnce() {
        if (!sink) {
            health.recordConfigurationError(AuditDrainStatus::MissingSink);
            return {.status = AuditDrainStatus::MissingSink};
        }
        if (!sink->isEnabled()) {
            health.recordConfigurationError(AuditDrainStatus::DisabledSink);
            return {.status = AuditDrainStatus::DisabledSink};
        }
        AuditDrainResult total;
        for (auto& source : ringList) {
            const auto one   = source.drain(*sink, health);
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
        return health.snapshot(pending);
    }

    /** @brief Independent channel for a sink or host to report a discovered post-hand-off loss. */
    void reportLoss(std::uint64_t count = 1) noexcept {
        health.reportLoss(count);
    }

private:
    struct RingSource {
        std::function<AuditDrainResult(sinks::AuditSink&, AuditHealth&)> drain;
        std::function<std::uint64_t()>                                   pending;
    };

    std::vector<RingSource> ringList;
    sinks::AuditSinkPtr     sink;
    AuditHealth             health;
};

}  // namespace mddlog::adapter
