/** @brief Host-driven audit lifecycle over a storage medium, outside the governed producer. */

export module mddlog.adapter.auditservice;

import std;
export import mddlog.adapter.auditdrain;
export import mddlog.adapter.auditstore;

export namespace mddlog::adapter {

enum class AuditCapacityPolicy : std::uint8_t { KeepPending, RelieveDeclared };

struct AuditServiceConfig {
    StorageConfig            storage;
    std::size_t              anchorRecordBound = 1;
    std::chrono::nanoseconds anchorAgeBound    = std::chrono::seconds{1};
    /** @brief Maximum records handed off from each registered ring per poll. Must be positive. */
    std::size_t         maxRecordsPerRing       = 1;
    std::size_t         maxAttemptsPerPoll      = std::numeric_limits<std::size_t>::max();
    std::size_t         maxAnchorStreamsPerPoll = std::numeric_limits<std::size_t>::max();
    AuditCapacityPolicy capacityPolicy          = AuditCapacityPolicy::KeepPending;
    /** @brief Minimum delay before retrying an unsuccessful anchor attempt for a stream. */
    std::chrono::nanoseconds anchorPeriod = std::chrono::seconds{1};
};

enum class AuditServiceConfigError : std::uint8_t { InvalidRecordBudget, InvalidAnchorPeriod, InvalidRetention, InvalidStorage, StartupFailed };

struct AuditServiceError {
    AuditServiceConfigError           issue        = AuditServiceConfigError::InvalidStorage;
    std::optional<StorageConfigError> storage      = std::nullopt;
    StorageIssue                      startupCause = StorageIssue::None;
};

enum class AuditServiceRegistration : std::uint8_t { Registered, InvalidStream, DuplicateStream, Capacity, LedgerIdentity, Started };

/** @brief Delivery and storage are separate snapshots, not a transaction across producer and consumer. */
struct AuditServiceHealth {
    AuditHealthSnapshot   delivery;
    StorageHealthSnapshot storage;
    std::uint64_t         unconfirmed      = 0;
    std::uint64_t         unanchored       = 0;
    std::uint64_t         callbackFailures = 0;
};

enum class AuditStopStatus : std::uint8_t { Completed, Pending, DeadlineExceeded, Degraded, InvalidBudget };

struct AuditStopOptions {
    std::size_t                             maxDrainPasses = 1;
    std::optional<std::chrono::nanoseconds> timeBudget;
};

struct AuditServiceStop {
    /** @brief Lifecycle ended. This does not promise durable confirmation or an accepted anchor. */
    bool                     closed = false;
    AuditServiceHealth       health;
    AuditStopStatus          status = AuditStopStatus::Pending;
    std::chrono::nanoseconds elapsed{};
};

/**
 * @brief One host-owned composition point for drain, sync, anchoring and orderly close.
 *
 * The medium, provider and registered rings must outlive this service. Registration finishes
 * before the first poll or concurrent health observation. poll() and stop() belong to the
 * single consumer; the host calls poll() even when producers are inactive, at a cadence that
 * satisfies its age bounds. Budgets bound attempted hand-offs and eligible anchor streams;
 * both schedules rotate fairly. Retention runs only with explicit standing permissions.
 * External calls remain synchronous and must supply their own timeout contract.
 * Clocks and host callbacks run on the consumer, must not reenter it, and must outlive it.
 *
 * Before stop(), the host stops producers and establishes quiescence. A bounded drain that
 * leaves pending events does not close the sink; the host can poll or retry stop(). A failed
 * stream remains failed according to the sink contract. Destruction detaches the sink but
 * performs no implicit I/O and makes no promise of graceful shutdown.
 */
class AuditService {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<AuditService>, AuditServiceError> create(StorageMedium& medium, AuditServiceConfig config) {
        if (config.maxRecordsPerRing == 0 || config.maxAttemptsPerPoll == 0 || config.maxAnchorStreamsPerPoll == 0)
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::InvalidRecordBudget});
        if (config.anchorPeriod <= std::chrono::nanoseconds::zero() || config.anchorAgeBound <= std::chrono::nanoseconds::zero()
            || config.anchorRecordBound == 0)
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::InvalidAnchorPeriod});
        if (config.capacityPolicy == AuditCapacityPolicy::RelieveDeclared
            && (!config.storage.ledger || (!config.storage.retention.rotate && !config.storage.retention.removeEnded)))
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::InvalidRetention});
        if (!config.storage.clock)
            config.storage.clock = [] {
                return std::chrono::steady_clock::now();
            };
        auto                                  service          = std::unique_ptr<AuditService>(new AuditService(config));
        const std::weak_ptr<AuditSinkAdapter> delivery         = service->consumer;
        const auto                            callbackFailures = service->callbackErrors;
        auto                                  hostLoss         = std::move(config.storage.reportLoss);
        config.storage.reportLoss                              = [delivery, callbackFailures, hostLoss = std::move(hostLoss)](std::uint64_t count) {
            if (const auto signal = delivery.lock())
                signal->reportLoss(count);
            if (hostLoss) {
                try {
                    hostLoss(count);
                } catch (...) {
                    callbackFailures->fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        auto made = PersistingAuditSink::create(medium, std::move(config.storage));
        if (!made)
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::InvalidStorage, .storage = made.error()});
        service->sink      = std::move(*made);
        const auto initial = service->sink->health();
        if (std::ranges::any_of(initial.streams, [](const auto& stream) {
                return stream.state == StreamStorageState::Failed;
            }))
            return std::unexpected(AuditServiceError{.issue = AuditServiceConfigError::StartupFailed, .startupCause = initial.lastIssue});
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
        if (!ring.hasValidIdentity()) {
            (void)consumer->addRing(ring);
            return AuditServiceRegistration::InvalidStream;
        }
        if (ring.identity() == ledgerIdentity)
            return AuditServiceRegistration::LedgerIdentity;
        if (std::ranges::find(identities, ring.identity()) != identities.end()) {
            (void)consumer->addRing(ring);
            return AuditServiceRegistration::DuplicateStream;
        }
        if (registered >= streamLimit)
            return AuditServiceRegistration::Capacity;
        identities.emplace_back(ring.identity());
        AuditRingRegistration answer = AuditRingRegistration::InvalidStream;
        try {
            answer = consumer->addRing(ring);
        } catch (...) {
            identities.pop_back();
            throw;
        }
        if (answer != AuditRingRegistration::Registered)
            identities.pop_back();
        switch (answer) {
            case AuditRingRegistration::Registered:
                ++registered;
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
        if (capacityPolicy == AuditCapacityPolicy::RelieveDeclared)
            (void)sink->relieve();
        const auto result = consumer->drainOnce(recordBudget, totalBudget);
        sink->tick();
        const auto  now      = clock();
        const auto  streams  = sink->health().streams;
        std::size_t attempts = 0;
        for (std::size_t visited = 0; visited < streams.size() && attempts < anchorBudget; ++visited) {
            anchorCursor      %= streams.size();
            const auto& stream = streams[anchorCursor];
            anchorCursor       = (anchorCursor + 1) % streams.size();
            if (stream.durablePosition <= stream.anchoredPosition)
                continue;
            if (stream.durablePosition - stream.anchoredPosition < anchorRecordBound
                && (!stream.unanchoredSince || now - *stream.unanchoredSince < anchorAgeBound))
                continue;
            const auto found = anchorAttempts.find(stream.streamId);
            if (found != anchorAttempts.end() && now - found->second < anchorPeriod)
                continue;
            if (sink->advanceAnchor(stream.streamId))
                anchorAttempts.erase(stream.streamId);
            else
                anchorAttempts[stream.streamId] = now;
            ++attempts;
        }
        return result;
    }

    [[nodiscard]] AuditServiceHealth health() const {
        AuditServiceHealth result{.delivery = consumer->healthSnapshot(), .storage = sink->health()};
        result.callbackFailures = callbackErrors->load(std::memory_order_relaxed);
        for (const auto& stream : result.storage.streams) {
            if (stream.isLedger)
                continue;
            result.unconfirmed += stream.appendedPosition - stream.durablePosition;
            result.unanchored  += stream.durablePosition - stream.anchoredPosition;
        }
        return result;
    }

    /** @brief Drain at most maxDrainPasses and close only after the quiescent rings are empty. */
    [[nodiscard]] AuditServiceStop stop(std::size_t maxDrainPasses = 1) {
        return stop(AuditStopOptions{.maxDrainPasses = maxDrainPasses, .timeBudget = std::nullopt});
    }

    /** @brief Soft deadline checked between synchronous phases; an external call can exceed it. */
    [[nodiscard]] AuditServiceStop stop(AuditStopOptions options) {
        if (options.timeBudget && *options.timeBudget <= std::chrono::nanoseconds::zero())
            return {.closed = closed, .health = health(), .status = AuditStopStatus::InvalidBudget};
        started            = true;
        const auto begin   = clock();
        const auto expired = [&] {
            return options.timeBudget && clock() - begin >= *options.timeBudget;
        };
        if (!closed) {
            for (std::size_t pass = 0; pass < options.maxDrainPasses && !expired() && consumer->healthSnapshot().pendingInRings != 0; ++pass)
                (void)poll();
            if (!expired() && consumer->healthSnapshot().pendingInRings == 0) {
                sink->close();
                closed = true;
            }
        }
        auto snapshot = health();
        auto status   = closed ? AuditStopStatus::Completed : AuditStopStatus::Pending;
        if (closed
            && (snapshot.unconfirmed != 0 || snapshot.unanchored != 0 || !snapshot.storage.integrity.empty() || snapshot.delivery.reportedLosses != 0
                || std::ranges::any_of(snapshot.storage.streams, [](const auto& stream) {
                       return stream.state == StreamStorageState::Failed || stream.appendedPosition > stream.durablePosition
                              || stream.durablePosition > stream.anchoredPosition;
                   })))
            status = AuditStopStatus::Degraded;
        if (expired())
            status = AuditStopStatus::DeadlineExceeded;
        return {.closed = closed, .health = std::move(snapshot), .status = status, .elapsed = clock() - begin};
    }

    /** @brief Low-level consumer access. The reference is valid only during the service lifetime. */
    [[nodiscard]] PersistingAuditSink& storageSink() noexcept {
        return *sink;
    }

private:
    explicit AuditService(const AuditServiceConfig& config)
        : consumer(std::make_shared<AuditSinkAdapter>()),
          recordBudget(config.maxRecordsPerRing),
          totalBudget(config.maxAttemptsPerPoll),
          anchorBudget(config.maxAnchorStreamsPerPoll),
          streamLimit(config.storage.maxProducerStreams),
          ledgerIdentity(config.storage.ledger ? config.storage.ledger->streamId : ""),
          capacityPolicy(config.capacityPolicy),
          anchorRecordBound(config.anchorRecordBound),
          anchorAgeBound(config.anchorAgeBound),
          anchorPeriod(config.anchorPeriod),
          clock(config.storage.clock) {}

    std::shared_ptr<AuditSinkAdapter>                            consumer;
    std::shared_ptr<PersistingAuditSink>                         sink;
    std::size_t                                                  recordBudget;
    std::size_t                                                  totalBudget;
    std::size_t                                                  anchorBudget;
    std::size_t                                                  streamLimit;
    std::size_t                                                  registered = 0;
    std::vector<std::string>                                     identities;
    std::string                                                  ledgerIdentity;
    AuditCapacityPolicy                                          capacityPolicy;
    std::size_t                                                  anchorCursor = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> anchorAttempts;
    std::shared_ptr<std::atomic<std::uint64_t>>                  callbackErrors = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::size_t                                                  anchorRecordBound;
    std::chrono::nanoseconds                                     anchorAgeBound;
    std::chrono::nanoseconds                                     anchorPeriod;
    std::function<std::chrono::steady_clock::time_point()>       clock;

    bool started = false;
    bool closed  = false;
};

}  // namespace mddlog::adapter
