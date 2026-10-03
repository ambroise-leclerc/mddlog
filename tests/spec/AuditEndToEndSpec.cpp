/**
 * @brief ADR-004 Decision 5 and milestone C of epic #11, end to end (issue #93): every validation scenario runs from producer admission through the audit
 * consumer, the persisting sink, the anchor provider and the log reader, and checks what the reader reports.
 *
 * The adversary of these scenarios rewrites, truncates or restores the medium between two starts, as Decision 1 assumes: it can do anything to the mutable
 * log, and nothing to the provider or the reader's retained position unless the scenario says so.
 */

import std;
import speclab;
import mddlog.adapter.auditdrain;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

#include "../framework/AuditLogRig.hpp"

namespace {

using namespace mddlog::spec::auditlog;
using mddlog::adapter::AuditRingRegistration;
using mddlog::adapter::AuditSinkAdapter;
using Operation   = InMemoryStorageMedium::Operation;
using Effect      = InMemoryStorageMedium::Effect;
using Unconfirmed = InMemoryStorageMedium::Unconfirmed;

constexpr std::string_view pump    = "device-42/boot-7/pump";
constexpr std::string_view pumpNew = "device-42/boot-8/pump";

[[nodiscard]] RawTime wallClock(std::int64_t seconds) {
    return RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::seconds{seconds}});
}

/**
 * @brief A device: one producer ring, the audit consumer, a persisting sink with a ledger over an in-memory medium, and an anchor provider that lives
 * outside the medium. Each start() is a new adapter start with new stream identities, as ADR-002 Decision 5 requires.
 */
struct Device {
    explicit Device(std::size_t capacity = 24, bool eligible = true)
        : medium(std::make_unique<InMemoryStorageMedium>(capacity, eligible)), segmentCount(capacity) {}
    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&)                 = delete;
    Device& operator=(Device&&)      = delete;
    ~Device() {
        stop();
    }

    [[nodiscard]] bool start(std::string_view producer, bool withProvider = true) {
        stop();
        ++session;
        ring    = std::make_unique<mddlog::core::AuditRing<64>>(producer);
        adapter = std::make_unique<AuditSinkAdapter>();
        StorageConfig config;
        config.segmentSize        = segmentBytes;
        config.segmentCount       = segmentCount;
        config.maxProducerStreams = 2;
        config.sync.recordBound   = 1;
        LedgerConfig ledger;
        ledger.streamId   = ledgerId();
        config.ledger     = ledger;
        config.provider   = withProvider ? &provider : nullptr;
        config.reportLoss = [this](std::uint64_t count) {
            if (adapter)
                adapter->reportLoss(count);
        };
        auto made = PersistingAuditSink::create(*medium, std::move(config));
        if (!made)
            return false;
        sink = *made;
        if (adapter->addRing(*ring) != AuditRingRegistration::Registered)
            return false;
        adapter->setSink(sink);
        next = 1;
        return true;
    }

    [[nodiscard]] std::string ledgerId() const {
        return "device-42/ledger/" + std::to_string(session);
    }

    /** @brief Admit `count` events of the producer and drain them to the sink. The events equal makeEvent(producer, k) byte for byte. */
    std::uint64_t produce(std::uint64_t count) {
        std::uint64_t admitted = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            detail = "step " + std::to_string(next);
            detail.resize(std::max<std::size_t>(detail.size(), 12), 'x');
            mddlog::core::AuditInput input;
            input.action = "therapy.rate.set";
            input.target = "pump/channel-A";
            input.detail = detail;
            if (ring->tryRecord(input).wasAdmitted()) {
                ++next;
                ++admitted;
            }
            (void)adapter->drainOnce();
        }
        return admitted;
    }

    /** @brief The adapter is stopped: nothing is closed, as after an abrupt end. The consumer goes first, since it holds the ring and the sink. */
    void stop() {
        adapter.reset();
        ring.reset();
        sink.reset();
    }

    /** @brief Power is lost: the sink is gone and the medium keeps what 9.2 allows. */
    void powerLoss(Unconfirmed policy) {
        stop();
        medium->restart(policy);
    }

    [[nodiscard]] LogReport read(RetainedPosition& retained, VerifierConfig verifierConfig = {}) {
        LogVerifier verifier{*medium, provider, retained, verifierConfig};
        return verifier.verify();
    }
    [[nodiscard]] LogReport read() {
        RetainedPosition retained;
        return read(retained);
    }

    std::unique_ptr<InMemoryStorageMedium>       medium;
    InMemoryAnchorProvider                       provider{"witness-1"};
    std::unique_ptr<mddlog::core::AuditRing<64>> ring;
    std::unique_ptr<AuditSinkAdapter>            adapter;
    std::shared_ptr<PersistingAuditSink>         sink;
    std::string                                  detail;
    std::uint64_t                                next    = 1;
    std::size_t                                  session = 0;
    /** @brief N declared to the sink: the capacity of the medium the device was built with. */
    std::size_t segmentCount;
};

/** @brief A copy of one stream's report, so that it outlives the log report it came from; an empty report when the stream is not listed. */
[[nodiscard]] StreamReport reportOf(const LogReport& report, std::string_view id) {
    const auto* found = report.find(id);
    return found == nullptr ? StreamReport{} : found->report;
}

/** @brief A copy of every segment the device's medium holds now, as an adversary keeps one to restore later. */
[[nodiscard]] std::unique_ptr<InMemoryStorageMedium> backupOf(Device& device) {
    auto copy = std::make_unique<InMemoryStorageMedium>(64);
    copySegments(*device.medium, *copy);
    return copy;
}

/**
 * @brief A rewriting adversary, while the adapter is stopped: the stream's records 1 … `keep` are written again with record `alterAt` changed (0 for none),
 * every digest recomputed so that each link checks; every other segment is copied unchanged.
 */
void rewrite(Device& device, std::string_view stream, std::uint64_t total, std::uint64_t alterAt, std::uint64_t keep) {
    device.stop();
    auto rewritten = std::make_unique<InMemoryStorageMedium>(64);
    for (const auto& info : device.medium->segments().value_or(std::vector<SegmentInfo>{})) {
        const auto bytes = device.medium->read(info.segment, 0, info.size).value_or(std::vector<std::uint8_t>{});
        const auto scan  = scanSegment(bytes);
        if (!scan.header.has_value() || scan.header->streamId == stream)
            continue;
        const SegmentHeader& header = *scan.header;
        const auto           open   = encodeSegmentOpening(header.streamId, header.segmentIndex, header.firstSequence);
        const auto           made   = rewritten->open(
            {.streamId = header.streamId, .segmentIndex = header.segmentIndex, .firstSequence = header.firstSequence, .bytes = open});
        (void)rewritten->append(made.segment, std::span{bytes}.subspan(open.size()));
    }
    std::vector<AuditEvent> events;
    for (std::uint64_t k = 1; k <= total; ++k)
        events.push_back(k == alterAt ? makeEvent(stream, k, 40) : makeEvent(stream, k));
    (void)forgeSegment(*rewritten, stream, events, 0, 0, std::nullopt, static_cast<std::size_t>(keep));
    device.medium = std::move(rewritten);
}

/** @brief A device that admitted `total` events and anchored the first `anchored` of them, then stopped without closing anything. */
void anchoredThenStopped(Device& device, std::uint64_t anchored, std::uint64_t total) {
    (void)device.start(pump);
    (void)device.produce(anchored);
    (void)device.sink->advanceAnchor(pump);
    (void)device.produce(total - anchored);
    device.stop();
}

const speclab::Register tampering{
    "A rewrite with recomputed digests, a truncated suffix and a restored old log are each found against the anchor, end to end",
    "integration",
    [] {
        return speclab::Test("audit-e2e-tampering")
            .Then("the device stores exactly what the producer admitted, and the reader verifies it against the anchor",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      checks.expect(device.start(pump), "started");
                      checks.expect(device.produce(15) == 15, "15 events admitted and drained");
                      checks.expect(device.sink->durablePosition(pump) == 15, "all of them durably confirmed");
                      checks.expect(device.sink->advanceAnchor(pump), "anchored at 15");
                      const auto stored = readStoredStream(*device.medium, pump);
                      checks.expect(stored.records().size() == 15
                                        && std::ranges::equal(stored.records().front().bytes, CanonicalRecord::encode(makeEvent(pump, 1))->bytes()),
                                    "the stored canonical bytes are the admitted event's");
                      const auto& report = reportOf(device.read(), pump);
                      checks.expect(report.verdict == Verdict::Anchored && report.firstRetained == 1 && report.anchoredThrough == 15,
                                    "Anchored, with its range 1 … 15");
                      checks.raise();
                  })
            .Then("rewrite with recomputation at k <= p: Altered, though every link checks",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      anchoredThenStopped(device, 10, 15);
                      rewrite(device, pump, 15, 4, 15);
                      const auto chain = detail::walkChain(pump, 0, chainInitialValue, readStoredStream(*device.medium, pump).records());
                      checks.expect(chain.finding == ChainFinding::Ok && chain.through == 15, "every internal link of the rewritten stream checks");
                      const auto& report = reportOf(device.read(), pump);
                      checks.expect(report.verdict == Verdict::Altered && report.cause == VerdictCause::AnchorDigestMismatch,
                                    "Altered: the recomputed H_p differs from the anchor's digest");
                      checks.expect(!report.anchoredThrough.has_value(), "nothing is reported anchored");
                      checks.raise();
                  })
            .Then("rewrite past the anchor: not detectable, and the report says coverage stops at p",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      anchoredThenStopped(device, 10, 15);
                      rewrite(device, pump, 15, 12, 15);
                      const auto& report = reportOf(device.read(), pump);
                      checks.expect(report.verdict == Verdict::Anchored && report.firstRetained == 1 && report.anchoredThrough == 10
                                        && report.unanchoredFrom == 11 && report.lastPresent == 15,
                                    "Anchored through 10 only; 11 … 15 internally consistent and unanchored");
                      checks.raise();
                  })
            .Then("suffix truncation below the anchor: Incomplete, the missing records named",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      anchoredThenStopped(device, 10, 15);
                      rewrite(device, pump, 15, 0, 7);
                      const auto& report = reportOf(device.read(), pump);
                      checks.expect(report.verdict == Verdict::Incomplete && report.cause == VerdictCause::LogEndsBeforeAnchor && report.lastPresent == 7,
                                    "Incomplete: records 8 … 10 are missing, the prefix 1 … 7 is internally consistent");
                      checks.raise();
                  })
            .Then("suffix truncation past the anchor: cannot be told from records never written, and coverage is stated",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      anchoredThenStopped(device, 10, 15);
                      rewrite(device, pump, 15, 0, 12);
                      const auto& report = reportOf(device.read(), pump);
                      checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == 10 && report.lastPresent == 12,
                                    "Anchored through 10; 11 … 12 unanchored; nothing says 13 … 15 ever existed");
                      checks.raise();
                  })
            .Then("old log restored with its old anchor: Rolled back with a retained position, and never an unqualified pass without one",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      checks.expect(device.start(pump), "started");
                      (void)device.produce(10);
                      (void)device.sink->advanceAnchor(pump);
                      auto       oldLog    = backupOf(device);
                      const auto oldAnchor = device.provider.snapshot();
                      (void)device.produce(5);
                      (void)device.sink->advanceAnchor(pump);
                      RetainedPosition retained;
                      checks.expect(reportOf(device.read(retained), pump).verdict == Verdict::Anchored, "verified through 15, and retained");
                      device.stop();
                      device.medium = std::move(oldLog);
                      device.provider.restore(oldAnchor);
                      const auto& rolled = reportOf(device.read(retained), pump);
                      checks.expect(rolled.verdict == Verdict::RolledBack && !rolled.anchoredThrough.has_value(), "Rolled back");
                      RetainedPosition none;
                      const auto&      naive = reportOf(device.read(none), pump);
                      checks.expect(naive.verdict == Verdict::Anchored && naive.anchoredThrough == 10 && naive.rollbackNotExcluded(),
                                    "without a retained position: Anchored through the old position, rollback not excluded");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register anchorStates{"A missing, unavailable or stale anchor is reported as such, and never as verified, end to end", "integration", [] {
                                         return speclab::Test("audit-e2e-anchor-states")
                                             .Then("no provider configured: internally consistent, unanchored",
                                                   [] {
                                                       speclab::core::Checks checks;
                                                       Device                device;
                                                       checks.expect(device.start(pump, false), "started with no provider");
                                                       (void)device.produce(5);
                                                       device.sink->close();
                                                       const auto& report = reportOf(device.read(), pump);
                                                       checks.expect(report.verdict == Verdict::Unanchored && report.cause == VerdictCause::NoAnchor
                                                                         && !report.anchoredThrough.has_value() && report.lastPresent == 5,
                                                                     "Unanchored, 1 … 5 internally consistent, never Anchored");
                                                       checks.raise();
                                                   })
                                             .Then("provider unavailable when the reader asks: anchor unavailable, reported apart from absence",
                                                   [] {
                                                       speclab::core::Checks checks;
                                                       Device                device;
                                                       checks.expect(device.start(pump), "started");
                                                       (void)device.produce(5);
                                                       device.sink->close();
                                                       device.provider.setAvailable(false);
                                                       const auto& report = reportOf(device.read(), pump);
                                                       checks.expect(report.verdict == Verdict::AnchorUnavailable
                                                                         && report.cause == VerdictCause::ProviderUnavailable,
                                                                     "anchor unavailable, not unanchored and not verified");
                                                       checks.raise();
                                                   })
                                             .Then("stale anchor: coverage stops at its position, and its age is stated against the declared bound",
                                                   [] {
                                                       speclab::core::Checks checks;
                                                       Device                device;
                                                       checks.expect(device.start(pump), "started");
                                                       device.provider.setClock(wallClock(1'000));
                                                       (void)device.produce(10);
                                                       (void)device.sink->advanceAnchor(pump);
                                                       (void)device.produce(4);
                                                       device.stop();
                                                       RetainedPosition retained;
                                                       const auto       late   = VerifierConfig{.maxAnchorAge     = std::chrono::seconds{60},
                                                                                                .verificationTime = wallClock(1'200)};
                                                       const auto&      report = reportOf(device.read(retained, late), pump);
                                                       checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == 10
                                                                         && report.unanchoredFrom == 11 && report.staleByPosition(),
                                                                     "stale by position: Anchored through 10 only");
                                                       checks.expect(report.age == AgeStatus::Stale, "stale by age, against the declared bound");
                                                       checks.raise();
                                                   })
                                             .Execute();
                                     }};

const speclab::Register resumption{
    "A restart after the storage was altered while stopped reports the alteration, and anchors nothing it reloaded",
    "integration",
    [] {
        return speclab::Test("audit-e2e-resumption")
            .Then("alteration at or before the anchor: the continuity check fails, a fault is reported, and the reader says Altered",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      anchoredThenStopped(device, 10, 13);
                      rewrite(device, pump, 13, 3, 13);
                      checks.expect(device.start(pumpNew), "the adapter restarts with new identities");
                      const RestartReport& restart = device.sink->restart();
                      checks.expect(std::ranges::any_of(restart.faults,
                                                        [](const IntegrityFault& fault) {
                                                            return fault.kind == IntegrityFaultKind::ContinuityFailed && fault.stream == pump;
                                                        }),
                                    "continuity check failed, reported through audit health");
                      checks.expect(restart.recoveredFailed == 1, "the old stream is cited in its Failed form");
                      checks.expect(!device.sink->advanceAnchor(pump), "nothing is ever advanced for the old stream");
                      const LatestAnswer latest = device.provider.latest(pump);
                      const auto*        anchor = std::get_if<Anchor>(&latest);
                      checks.expect(anchor != nullptr && anchor->position == 10, "the provider still holds the anchor at 10");
                      (void)device.produce(3);
                      const auto report = device.read();
                      checks.expect(reportOf(report, pump).verdict == Verdict::Altered, "the old stream is Altered");
                      checks.expect(reportOf(report, pumpNew).firstRetained == 1 && reportOf(report, pumpNew).lastPresent == 3,
                                    "the new instance starts at 1: the discontinuity is visible");
                      checks.raise();
                  })
            .Then("alteration only past the anchor: the check passes, and the reloaded records stay unanchored",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      anchoredThenStopped(device, 10, 13);
                      rewrite(device, pump, 13, 12, 13);
                      checks.expect(device.start(pumpNew), "restarted");
                      checks.expect(std::ranges::none_of(device.sink->restart().faults,
                                                         [](const IntegrityFault& fault) {
                                                             return fault.kind == IntegrityFaultKind::ContinuityFailed;
                                                         }),
                                    "no continuity fault: the alteration is not detectable");
                      const auto& report = reportOf(device.read(), pump);
                      checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == 10 && report.unanchoredFrom == 11,
                                    "Anchored through 10, records past it internally consistent and unanchored");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register powerAndDurability{
    "Power loss while writing never loses a durably confirmed record, and nothing is confirmed before the storage contract allows it",
    "integration",
    [] {
        return speclab::Test("audit-e2e-durability")
            .Then("a cut before, during or after an append: trailing bytes are reported, no record at or below the durable position is missing",
                  [] {
                      speclab::core::Checks checks;
                      for (const Effect effect : {Effect::CutBefore, Effect::CutPartial, Effect::CutAfter}) {
                          Device device;
                          checks.expect(device.start(pump), "started");
                          (void)device.produce(4);
                          device.medium->inject(
                              {.operation = Operation::Append, .ordinal = device.medium->calls(Operation::Append) + 2, .effect = effect, .partialBytes = 9});
                          (void)device.produce(4);
                          const std::uint64_t durable = device.sink->durablePosition(pump);
                          device.powerLoss(Unconfirmed::Kept);
                          const auto& report = reportOf(device.read(), pump);
                          checks.expect(report.verdict == Verdict::Unanchored && report.lastPresent >= durable && durable >= 4,
                                        "the reader finds every durably confirmed record and no finding");
                          checks.expect(device.start(pumpNew), "the next start succeeds");
                          const bool trailing = std::ranges::any_of(device.sink->recovery().findings, [](const RecoveryFinding& finding) {
                              return finding.kind == RecoveryFindingKind::TrailingBytes && finding.streamId == pump;
                          });
                          checks.expect(trailing == (effect == Effect::CutPartial), "a partial frame is reported as trailing bytes, and read as no record");
                      }
                      checks.raise();
                  })
            .Then("a medium that cannot confirm durability: nothing is confirmed, nothing is anchored, and the reader says unanchored",
                  [] {
                      speclab::core::Checks checks;
                      Device                device{24, false};
                      checks.expect(device.start(pump), "started on a medium that answers Unsupported");
                      checks.expect(device.produce(5) == 5, "admitted and handed off");
                      checks.expect(device.sink->durablePosition(pump) == 0 && !device.sink->durableClaim(pump).has_value(), "no durable position, no claim");
                      checks.expect(!device.sink->advanceAnchor(pump) && std::holds_alternative<AnchorAbsent>(device.provider.latest(pump)),
                                    "no anchor can cover an unconfirmed record");
                      checks.expect(reportOf(device.read(), pump).verdict == Verdict::Unanchored, "Unanchored");
                      checks.raise();
                  })
            .Then("a failing storage is observable in the sink's health and as a loss after admission in the consumer's",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      checks.expect(device.start(pump), "started");
                      (void)device.produce(3);
                      device.medium->inject({.operation = Operation::Sync, .ordinal = device.medium->calls(Operation::Sync) + 1, .effect = Effect::Fail});
                      (void)device.produce(1);
                      const auto health = device.sink->health();
                      const auto stream = std::ranges::find(health.streams, pump, &StreamStorageHealth::streamId);
                      checks.expect(stream != health.streams.end() && stream->state == StreamStorageState::Failed && stream->cause == StorageIssue::SyncFailed
                                        && stream->durablePosition == 3,
                                    "the instance failed on its sync, durable through 3");
                      checks.expect(device.adapter->healthSnapshot().reportedLosses == 1, "the event never confirmed is a reported loss");
                      (void)device.produce(1);
                      checks.expect(device.adapter->healthSnapshot().pendingInRings == 1, "a later event stays in the ring: nothing is skipped");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register boundaries{
    "A restart and a rotation are reported as boundaries, never as continuity, end to end",
    "integration",
    [] {
        return speclab::Test("audit-e2e-boundaries")
            .Then("restart: a new chain, the new ledger cites the old one, and the old stream is closed at its last record",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      checks.expect(device.start(pump), "first start");
                      (void)device.produce(5);
                      device.sink->close();
                      const std::string firstLedger = device.ledgerId();
                      checks.expect(device.start(pumpNew), "restart with new identities");
                      (void)device.produce(3);
                      device.sink->close();
                      const auto report = device.read();
                      checks.expect(report.has(BoundaryKind::ClosedAt, pump), "the old stream: closed at 5");
                      checks.expect(report.has(BoundaryKind::PredecessorMatched, device.ledgerId()), "the new ledger cites the old one, matched");
                      checks.expect(reportOf(report, pump).verdict == Verdict::Anchored && reportOf(report, pump).anchoredThrough == 5, "old stream: 1 … 5");
                      checks.expect(reportOf(report, pumpNew).firstRetained == 1 && reportOf(report, pumpNew).anchoredThrough == 3,
                                    "new stream: its own chain from 1");
                      checks.expect(reportOf(report, firstLedger).verdict == Verdict::Anchored, "each ledger is verified like any stream");
                      checks.raise();
                  })
            .Then("rotation: coverage starts after the trim, from the trim's digest",
                  [] {
                      speclab::core::Checks checks;
                      Device                device;
                      checks.expect(device.start(pump), "started");
                      (void)device.produce(60);
                      checks.expect(segmentsOf(*device.medium, pump).size() >= 3, "the stream spans several segments");
                      checks.expect(device.sink->advanceAnchor(pump), "anchored at 60");
                      const auto trimmed = device.sink->trimPrefix(pump);
                      checks.expect(trimmed.outcome == RetentionOutcome::Trimmed && trimmed.trimmedThrough > 0, "a whole-segment prefix is removed");
                      const auto  report = device.read();
                      const auto& stream = reportOf(report, pump);
                      checks.expect(stream.verdict == Verdict::Anchored && stream.firstRetained == trimmed.trimmedThrough + 1 && stream.anchoredThrough == 60,
                                    "Anchored from q + 1 through 60");
                      checks.expect(report.has(BoundaryKind::RemovedUnderRetention, pump), "the removal is stated, with its trim");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
