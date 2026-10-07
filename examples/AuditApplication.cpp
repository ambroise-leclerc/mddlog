/** @brief Portable composition study: business emission is independent of maintenance (#116). */
import std;
import mddlog.adapter.auditservice;
import mddlog.core.auditbinding;

namespace {
template <std::size_t Capacity>
bool inspect(mddlog::core::AuditBinding<Capacity>& audit) {
    return audit.record(mddlog::core::AuditPhase::Requested, mddlog::core::RawTime::unavailable()).wasAdmitted();
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): unexpected allocation failure terminates this demonstration.
int main() {
    using namespace mddlog::adapter;
    using namespace std::chrono_literals;
    // These doubles demonstrate the portable API; real deployment uses qualified storage and an independent provider.
    InMemoryStorageMedium      medium{16};
    InMemoryAnchorProvider     provider{"demonstration"};
    mddlog::core::AuditRing<4> ring{"inventory/demo-boot-1"};
    StorageConfig              storage;
    storage.segmentSize        = 8192;
    storage.segmentCount       = 16;
    storage.maxProducerStreams = 1;
    storage.ledger             = LedgerConfig{.streamId = "ledger/demo-boot-1", .time = {}};
    storage.provider           = &provider;
    storage.sync               = {.recordBound = 4, .ageBound = 10ms};
    auto service               = AuditService::create(medium, {.storage = storage, .maxAttemptsPerPoll = 4, .maxAnchorStreamsPerPoll = 2});
    if (!service || (*service)->addRing(ring) != AuditServiceRegistration::Registered)
        return 1;
    const auto description = mddlog::core::AuditDescription::create({.action = "inventory.inspect"});
    const auto context     = mddlog::core::AuditContext::create({.target = "warehouse"});
    if (!description || !context)
        return 1;
    mddlog::core::AuditBinding audit{ring, *description, *context};
    if (!inspect(audit))
        return 1;
    (void)(*service)->poll();
    // The host continues polling during inactivity and stops producers before this ordered shutdown.
    const auto stopped = (*service)->stop(AuditStopOptions{.maxDrainPasses = 4, .timeBudget = 100ms});
    std::println("Handed off: {}; unconfirmed: {}; unanchored: {}; pending: {}",
                 stopped.health.delivery.handedOff,
                 stopped.health.unconfirmed,
                 stopped.health.unanchored,
                 stopped.health.delivery.pendingInRings);
    return stopped.closed && stopped.status == AuditStopStatus::Completed ? 0 : 1;
}
