/** @brief Host-driven audit lifecycle over a storage medium, outside the governed producer. */

export module mddlog.adapter.auditservice;

import std;
export import mddlog.adapter.auditdrain;
export import mddlog.adapter.auditstore;

export namespace mddlog::adapter {

struct AuditServiceConfig {
    StorageConfig storage;
    /** @brief Maximum records handed off from each registered ring per poll. Must be positive. */
    std::size_t maxRecordsPerRing = 1;
    /** @brief Period for attempts against the configured provider, including retries after unavailability. */
    std::chrono::nanoseconds anchorPeriod = std::chrono::seconds{1};
};

enum class AuditServiceConfigError : std::uint8_t { InvalidRecordBudget, InvalidAnchorPeriod, InvalidStorage };

struct AuditServiceError {
    AuditServiceConfigError           issue   = AuditServiceConfigError::InvalidStorage;
    std::optional<StorageConfigError> storage = std::nullopt;
};

enum class AuditServiceRegistration : std::uint8_t { Registered, InvalidStream, DuplicateStream, Started };

/** @brief Delivery and storage are separate snapshots, not a transaction across producer and consumer. */
struct AuditServiceHealth {
    AuditHealthSnapshot   delivery;
    StorageHealthSnapshot storage;
};

struct AuditServiceStop {
    /** @brief Lifecycle ended. This does not promise durable confirmation or an accepted anchor. */
    bool               closed = false;
    AuditServiceHealth health;
};

/**
 * @brief One host-owned composition point for drain, sync, anchoring and orderly close.
 *
 * The medium, provider and registered rings must outlive this service. Registration finishes
 * before the first poll or concurrent health observation. poll() and stop() belong to the
 * single consumer; the host calls poll() even when producers are inactive, at a cadence that
 * satisfies its age bounds. Each poll visits all rings with the same per-ring budget, ticks
 * storage and attempts eligible anchors at anchorPeriod. External calls remain synchronous:
 * the record budget is not a wall-clock bound. No thread or retention policy is introduced.
 *
 * Before stop(), the host stops producers and establishes quiescence. A bounded drain that
 * leaves pending events does not close the sink; the host can poll or retry stop(). A failed
 * stream remains failed according to the sink contract. Destruction detaches the sink but
 * performs no implicit I/O and makes no promise of graceful shutdown.
 */
class AuditService {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<AuditService>, AuditServiceError> create(StorageMedium& medium, AuditServiceConfig config) {
        if (config.maxRecordsPerRing == 0)
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::InvalidRecordBudget});
        if (config.anchorPeriod <= std::chrono::nanoseconds::zero())
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::InvalidAnchorPeriod});
        if (!config.storage.clock)
            config.storage.clock = [] {
                return std::chrono::steady_clock::now();
            };
        auto                                  service  = std::unique_ptr<AuditService>(new AuditService(config));
        const std::weak_ptr<AuditSinkAdapter> delivery = service->consumer;
        auto                                  hostLoss = std::move(config.storage.reportLoss);
        config.storage.reportLoss                      = [delivery, hostLoss = std::move(hostLoss)](std::uint64_t count) {
            if (const auto signal = delivery.lock())
                signal->reportLoss(count);
            if (hostLoss)
                hostLoss(count);
        };
        auto made = PersistingAuditSink::create(medium, std::move(config.storage));
        if (!made)
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::InvalidStorage, .storage = made.error()});
        service->sink = std::move(*made);
        service->consumer->setSink(service->sink);
        return service;
    }

    AuditService(const AuditService&)            = delete;
    AuditService& operator=(const AuditService&) = delete;
    AuditService(AuditService&&)                 = delete;
    AuditService& operator=(AuditService&&)      = delete;
    ~AuditService() {
        consumer->setSink(nullptr);
    }

    template <std::size_t Capacity>
    [[nodiscard]] AuditServiceRegistration addRing(core::AuditRing<Capacity>& ring) {
        if (started)
            return AuditServiceRegistration::Started;
        switch (consumer->addRing(ring)) {
            case AuditRingRegistration::Registered:
                return AuditServiceRegistration::Registered;
            case AuditRingRegistration::InvalidStream:
                return AuditServiceRegistration::InvalidStream;
            case AuditRingRegistration::DuplicateStream:
                return AuditServiceRegistration::DuplicateStream;
        }
        return AuditServiceRegistration::InvalidStream;
    }

    [[nodiscard]] AuditDrainResult poll() {
        started = true;
        if (closed)
            return {};
        const auto result = consumer->drainOnce(recordBudget);
        sink->tick();
        const auto now = clock();
        if (!lastAnchorAttempt || now - *lastAnchorAttempt >= anchorPeriod) {
            for (const auto& stream : sink->health().streams)
                (void)sink->advanceAnchor(stream.streamId);
            lastAnchorAttempt = now;
        }
        return result;
    }

    [[nodiscard]] AuditServiceHealth health() const {
        return {.delivery = consumer->healthSnapshot(), .storage = sink->health()};
    }

    /** @brief Drain at most maxDrainPasses and close only after the quiescent rings are empty. */
    [[nodiscard]] AuditServiceStop stop(std::size_t maxDrainPasses = 1) {
        started = true;
        if (!closed) {
            for (std::size_t pass = 0; pass < maxDrainPasses && consumer->healthSnapshot().pendingInRings != 0; ++pass)
                (void)poll();
            if (consumer->healthSnapshot().pendingInRings == 0) {
                sink->close();
                closed = true;
            }
        }
        return {.closed = closed, .health = health()};
    }

private:
    explicit AuditService(const AuditServiceConfig& config)
        : consumer(std::make_shared<AuditSinkAdapter>()),
          recordBudget(config.maxRecordsPerRing),
          anchorPeriod(config.anchorPeriod),
          clock(config.storage.clock) {}

    std::shared_ptr<AuditSinkAdapter>                      consumer;
    std::shared_ptr<PersistingAuditSink>                   sink;
    std::size_t                                            recordBudget;
    std::chrono::nanoseconds                               anchorPeriod;
    std::function<std::chrono::steady_clock::time_point()> clock;
    std::optional<std::chrono::steady_clock::time_point>   lastAnchorAttempt;
    bool                                                   started = false;
    bool                                                   closed  = false;
};

}  // namespace mddlog::adapter
