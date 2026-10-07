/** @brief Permitted audit observers and host lifecycle budgets under concurrent saturation. */
import std;
import mddlog.adapter.auditservice;

namespace {
using Clock = std::chrono::steady_clock;
void emit(std::string_view name, std::chrono::nanoseconds elapsed, std::uint64_t units) {
    std::cout << name << ',' << elapsed.count() << ',' << units << '\n';
}
}  // namespace
int main() try {
    using namespace mddlog::adapter;
    constexpr std::size_t   producers   = 32;
    constexpr std::uint64_t perProducer = 512;
    constexpr std::uint64_t total       = producers * perProducer;
    AuditResourceLimits     profile;
    profile.maxSegments        = 512;
    profile.maxSegmentBytes    = 32768;
    profile.maxTotalBytes      = std::uint64_t{16} * 1024 * 1024;
    profile.readChunkBytes     = 4096;
    profile.maxReadBytes       = std::uint64_t{32} * 1024 * 1024;
    profile.maxStreams         = 256;
    profile.maxRecords         = 65536;
    profile.maxProviderEntries = 2048;
    profile.maxProviderCalls   = 8192;
    profile.maxIntegrityFaults = 2048;
    InMemoryStorageMedium  medium{512};
    InMemoryAnchorProvider provider{"service/witness"};
    provider.setAvailable(false);
    AuditServiceConfig config;
    config.storage.segmentCount       = 512;
    config.storage.segmentSize        = 32768;
    config.storage.maxProducerStreams = producers;
    config.storage.sync.recordBound   = 128;
    config.storage.resources          = profile;
    config.storage.provider           = &provider;
    config.storage.ledger             = LedgerConfig{.streamId = "service/ledger"};
    config.maxRecordsPerRing          = 4;
    config.maxAttemptsPerPoll         = 64;
    config.maxAnchorStreamsPerPoll    = 4;
    config.anchorRecordBound          = 128;
    config.anchorPeriod               = std::chrono::milliseconds{1};
    using Ring                        = mddlog::core::AuditRing<8>;
    std::array<std::unique_ptr<Ring>, producers> rings;
    auto                                         made = AuditService::create(medium, config);
    if (!made)
        return 2;
    auto& service = **made;
    for (std::size_t i = 0; i < producers; ++i) {
        rings.at(i) = std::make_unique<Ring>("service/producer/" + std::to_string(i));
        if (service.addRing(*rings.at(i)) != AuditServiceRegistration::Registered)
            return 3;
    }
    std::latch                filled{producers};
    std::latch                beginDrain{1};
    std::atomic<bool>         invalid{false};
    std::vector<std::jthread> writers;
    writers.reserve(producers);
    for (std::size_t i = 0; i < producers; ++i) {
        writers.emplace_back([&, i](const std::stop_token& stop) {
            auto&         ring     = *rings.at(i);
            std::uint64_t admitted = 0;
            for (; admitted < 8; ++admitted)
                if (!ring.tryRecord({.action = "service.measure", .target = "fixture"}).wasAdmitted())
                    invalid.store(true);
            const auto full = ring.tryRecord({.action = "service.measure", .target = "fixture"});
            if (full.wasAdmitted() || !full.refusal() || full.refusal()->reason != mddlog::core::AuditRefusalReason::RingFull)
                invalid.store(true);
            filled.count_down();
            beginDrain.wait();
            while (admitted < perProducer && !stop.stop_requested()) {
                const auto result = ring.tryRecord({.action = "service.measure", .target = "fixture"});
                if (result.wasAdmitted())
                    ++admitted;
                else if (!result.refusal() || result.refusal()->reason != mddlog::core::AuditRefusalReason::RingFull)
                    invalid.store(true);
                std::this_thread::yield();
            }
        });
    }
    std::chrono::nanoseconds maximumObservation{};
    std::uint64_t            observations = 0;
    std::jthread             observer([&](const std::stop_token& stop) {
        std::uint64_t handedOff = 0;
        while (!stop.stop_requested()) {
            const auto start  = Clock::now();
            const auto health = service.health();
            if (health.delivery.handedOff < handedOff || health.storage.streams.size() > producers + 1 || health.positionOrderViolations != 0)
                invalid.store(true);
            handedOff = health.delivery.handedOff;
            (void)service.storageSink().health();
            for (const auto& ring : rings) {
                const auto claim = service.storageSink().durableClaim(ring->identity());
                if (claim && (claim->position > service.storageSink().durablePosition(ring->identity()) || claim->position > perProducer))
                    invalid.store(true);
                const auto acknowledged = ring->acknowledgedCount();
                if (acknowledged > ring->admittedCount())
                    invalid.store(true);
                (void)ring->refusalCount();
            }
            maximumObservation = std::max(maximumObservation, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start));
            ++observations;
            std::this_thread::yield();
        }
    });
    filled.wait();
    const auto beginning = Clock::now();
    beginDrain.count_down();
    std::chrono::nanoseconds maximumPoll{};
    std::uint64_t            polls = 0;
    while (service.health().delivery.handedOff < total && Clock::now() - beginning < std::chrono::seconds{30}) {
        if (service.health().delivery.handedOff >= total / 2)
            provider.setAvailable(true);
        const auto start  = Clock::now();
        const auto result = service.poll();
        if (result.attempted > config.maxAttemptsPerPoll)
            invalid.store(true);
        maximumPoll = std::max(maximumPoll, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start));
        ++polls;
        std::this_thread::yield();
    }
    for (auto& writer : writers) {
        writer.request_stop();
        writer.join();
    }
    observer.request_stop();
    observer.join();
    provider.setAvailable(true);
    const auto recoveryStart = Clock::now();
    bool       unanchored    = true;
    while (unanchored && Clock::now() - recoveryStart < std::chrono::seconds{5}) {
        (void)service.poll();
        const auto health = service.health();
        unanchored        = std::ranges::any_of(health.storage.streams, [](const auto& stream) {
            return stream.unanchoredCount() != 0;
        });
    }
    emit("service_reconcile", std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - recoveryStart), 1);
    const auto closeStart = Clock::now();
    const auto stopped    = service.stop({.maxDrainPasses = 4, .timeBudget = std::chrono::seconds{1}});
    emit("service_stop", std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - closeStart), 1);
    emit("service_total", std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - beginning), total);
    emit("service_poll_max", maximumPoll, polls);
    emit("service_observer_max", maximumObservation, observations);
    std::cout << "service_streams,0," << stopped.health.storage.streams.size() << '\n';
    std::cout << "service_refusals,0," << stopped.health.delivery.ringFullRefusals << '\n';
    if (invalid.load() || observations == 0 || stopped.status != AuditStopStatus::Completed || !stopped.closed || stopped.health.delivery.handedOff != total
        || stopped.health.delivery.pendingInRings != 0 || stopped.health.storage.resourceIssue != AuditResourceIssue::None)
        return 4;
    return 0;
} catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 5;
}
