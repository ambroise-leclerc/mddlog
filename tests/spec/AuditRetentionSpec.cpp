/** @brief ADR-004 Decision 10.4 and 10.5: rotation, retention, interrupted removals, and what a reader reports at those boundaries (issue #92). */

import std;
import speclab;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

#include "../framework/AuditLogRig.hpp"

namespace {

using namespace mddlog::spec::auditlog;
using Operation = InMemoryStorageMedium::Operation;
using Effect    = InMemoryStorageMedium::Effect;

constexpr std::string_view streamP = "device-42/boot-7";
constexpr std::size_t      big     = 160;

/** @brief Records of 160 bytes of detail that one segment holds. */
[[nodiscard]] std::uint64_t perSegment() {
    static const std::uint64_t value = framesPerSegment(streamP, big);
    return value;
}

/** @brief Feed P up to `total` records of the large size, advancing P's anchor to `anchorAt` on the way. Every record is confirmed as it is appended. */
void feedAnchored(Rig& rig, std::uint64_t anchorAt, std::uint64_t total) {
    (void)rig.feed(streamP, 1, anchorAt, big);
    if (anchorAt != 0)
        (void)rig.sink->advanceAnchor(streamP);
    (void)rig.feed(streamP, anchorAt + 1, total - anchorAt, big);
}

[[nodiscard]] std::size_t trimsOf(Rig& rig, std::string_view ledger) {
    const LogAnalysis log = LogAnalysis::read(rig.medium);
    const auto*       one = log.ledger(ledger);
    return one == nullptr ? 0 : one->trims.size();
}

[[nodiscard]] bool hasFaultOf(std::span<const IntegrityFault> faults, IntegrityFaultKind kind, std::string_view stream) {
    return std::ranges::any_of(faults, [&](const IntegrityFault& fault) {
        return fault.kind == kind && fault.stream == stream;
    });
}

[[nodiscard]] const StreamBoundaryReport* reportOf(const LogReport& report, std::string_view id) {
    return report.find(id);
}

const speclab::Register rotation{
    "Rotation removes a whole-segment prefix up to the anchor, after a durably confirmed trim record",
    "integration",
    [] {
        return speclab::Test("audit-rotation")
            .Then("with q below the anchor: the latest whole segment that ends before it, and the reader verifies from the trim's digest",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      const std::uint64_t p         = (3 * segFrames) + 2;  // inside the fourth segment
                      feedAnchored(*rig, p, (4 * segFrames) + 2);
                      checks.expect(segmentsOf(rig->medium, streamP).size() == 5, "five segments");
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Trimmed && result.trimmedThrough == 3 * segFrames && result.segmentsReclaimed == 3
                                        && !result.retired,
                                    "q is the last record of the third segment, three segments reclaimed");
                      const auto held = heldOf(rig->medium, streamP);
                      checks.expect(held.first == (3 * segFrames) + 1 && held.last == (4 * segFrames) + 2, "records q + 1 … the last are kept");
                      const LogAnalysis log  = LogAnalysis::read(rig->medium);
                      const auto*       mine = log.ledger("ledger/1");
                      checks.expect(mine != nullptr && mine->trims.size() == 1 && mine->trims[0].stream == streamP && mine->trims[0].position == 3 * segFrames
                                        && mine->trims[0].digest == chainDigestAt(streamP, 3 * segFrames, big),
                                    "the trim record carries q and H_q");
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::Rotated && stream->report.verdict == Verdict::Anchored
                                        && stream->report.firstRetained == (3 * segFrames) + 1 && stream->report.anchoredThrough == p
                                        && stream->report.unanchoredFrom == p + 1,
                                    "Anchored through p, from q + 1");
                      const auto removed = report.notesOf(BoundaryKind::RemovedUnderRetention);
                      checks.expect(removed.size() == 1 && removed[0].position == 3 * segFrames && removed[0].ledger == "ledger/1",
                                    "records 1 … q removed under retention");
                      const auto health = rig->sink->health();
                      checks.expect(health.counters.trimsRecorded == 1 && health.counters.segmentsReclaimed == 3, "counted");
                      checks.raise();
                  })
            .Then("with q equal to the anchor: the trim's digest is compared with the anchor's and no anchored record is left in the log",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      feedAnchored(*rig, 3 * segFrames, (4 * segFrames) + 2);
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Trimmed && result.trimmedThrough == 3 * segFrames, "q is p");
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Anchored && stream->report.anchoredThrough == 3 * segFrames
                                        && stream->report.firstRetained == (3 * segFrames) + 1 && stream->report.unanchoredFrom == (3 * segFrames) + 1,
                                    "Anchored at p with every record past it unanchored");
                      checks.raise();
                  })
            .Then("when no segment ends at or before the anchor, nothing is written and nothing is removed: the adapter never trims past it",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      feedAnchored(*rig, segFrames - 2, segFrames + 3);
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::NothingToTrim, "the first segment ends at segFrames, above p = segFrames - 2");
                      checks.expect(trimsOf(*rig, "ledger/1") == 0 && heldOf(rig->medium, streamP).first == 1 && segmentsOf(rig->medium, streamP).size() == 2,
                                    "no trim record, no segment removed");
                      checks.expect(rig->sink->health().counters.retentionRefused == 1, "counted as a refusal");
                      checks.raise();
                  })
            .Then("the open segment and the last segment of a stream are never trimmed",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      feedAnchored(*rig, 2 * segFrames, 2 * segFrames);  // two full segments, anchor at the last record
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Trimmed && result.trimmedThrough == segFrames && result.segmentsReclaimed == 1,
                                    "only the first segment goes");
                      checks.expect(heldOf(rig->medium, streamP).first == segFrames + 1 && heldOf(rig->medium, streamP).last == 2 * segFrames,
                                    "the last segment stays");
                      checks.expect(rig->sink->trimPrefix(streamP).outcome == RetentionOutcome::NothingToTrim, "nothing more to trim");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register anchorBound{
    "Rotation relies only on an accepted anchor: none, an unavailable provider, and a deployment with no provider",
    "integration",
    [] {
        return speclab::Test("audit-rotation-bound")
            .Then("a provider that has accepted no anchor for the stream: nothing is removed",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      (void)rig->feed(streamP, 1, (3 * segFrames) + 1, big);
                      checks.expect(rig->sink->trimPrefix(streamP).outcome == RetentionOutcome::NoAnchor, "no p, no rotation");
                      checks.expect(trimsOf(*rig, "ledger/1") == 0 && heldOf(rig->medium, streamP).first == 1, "nothing written, nothing removed");
                      checks.raise();
                  })
            .Then("an unavailable provider: only the anchor this adapter holds from its own advance, and otherwise it waits",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      feedAnchored(*rig, 2 * segFrames, (3 * segFrames) + 1);
                      rig->provider.inner.setAvailable(false);
                      const auto own = rig->sink->trimPrefix(streamP);
                      checks.expect(own.outcome == RetentionOutcome::Trimmed && own.trimmedThrough == 2 * segFrames,
                                    "the adapter's own advance bounds it at 2F");
                      rig->sink->close();
                      rig->provider.inner.setAvailable(true);
                      checks.expect(rig->start(), "restarted");
                      rig->provider.inner.setAvailable(false);
                      const auto earlier = rig->sink->trimPrefix(streamP);
                      checks.expect(earlier.outcome == RetentionOutcome::ProviderUnavailable,
                                    "an earlier stream: it has no anchor of its own to rely on, so it waits");
                      checks.raise();
                  })
            .Then("a deployment with no provider trims without the bound, and its streams are reported unanchored",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(rig->config(), false), "started with no provider");
                      const std::uint64_t segFrames = perSegment();
                      (void)rig->feed(streamP, 1, (3 * segFrames) + 1, big);
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Trimmed && result.trimmedThrough == 3 * segFrames, "everything but the open segment");
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Unanchored && stream->report.firstRetained == (3 * segFrames) + 1,
                                    "internally consistent, unanchored, from q + 1");
                      checks.raise();
                  })
            .Then("a trim past the anchor that leaves records is a finding: the stream cannot be verified",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(rig->config(), false), "no provider at the time of the trim");
                      const std::uint64_t segFrames = perSegment();
                      (void)rig->feed(streamP, 1, (3 * segFrames) + 1, big);
                      checks.expect(rig->sink->trimPrefix(streamP).outcome == RetentionOutcome::Trimmed, "trimmed through 3F");
                      Anchor early;
                      early.anchorFormat     = anchorFormatVersion;
                      early.canonicalVersion = canonicalContractVersion;
                      early.streamId         = std::string{streamP};
                      early.position         = segFrames;  // the anchor the provider holds is below the trim
                      early.digest           = chainDigestAt(streamP, segFrames, big);
                      early.providerId       = "witness-1";
                      early.counter          = 1;
                      rig->provider.inner.inject(early);
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::CannotVerify
                                        && stream->report.cause == VerdictCause::TrimPastAnchor,
                                    "Cannot verify, naming the cause");
                      checks.expect(report.has(BoundaryKind::TrimPastAnchor, streamP), "and a boundary note");
                      checks.raise();
                  })
            .Then("a trim at or past a retained checkpoint ends the protection that checkpoint gave its prefix, and the report says so",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      (void)rig->feed(streamP, 1, segFrames + 1, big);
                      (void)rig->sink->advanceAnchor(streamP);
                      RetainedPosition retained;
                      const auto       first = readLog(*rig, retained);
                      checks.expect(reportOf(first, streamP)->report.verdict == Verdict::Anchored && retained.anchor(streamP).has_value(),
                                    "the reader retains the checkpoint");
                      (void)rig->feed(streamP, segFrames + 2, (3 * segFrames), big);
                      (void)rig->sink->advanceAnchor(streamP);
                      checks.expect(rig->sink->trimPrefix(streamP).outcome == RetentionOutcome::Trimmed, "trimmed past the checkpoint");
                      const auto second = readLog(*rig, retained);
                      checks.expect(reportOf(second, streamP)->report.verdict == Verdict::Anchored
                                        && reportOf(second, streamP)->report.retained == RetainedOutcome::CheckpointTrimmed,
                                    "still Anchored at the new anchor, and the checkpoint's protection is reported as ended");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register fullMedium{
    "A full medium still has room for the trim, and a full stream continues without a gap once a reclaim frees a segment",
    "integration",
    [] {
        return speclab::Test("audit-rotation-full")
            .Then("the ledger draws on the reserve when only the reserve is free, the trim is written, and the stream leaves the full state with no gap",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      std::uint64_t       fed       = 0;
                      while (rig->sink->accept(makeEvent(streamP, fed + 1, big)))
                          ++fed;
                      const auto full = rig->sink->health();
                      checks.expect(full.counters.fullEntries == 1 && full.freeSegmentsBeyondReserve == 0 && fed > 5 * segFrames,
                                    "the stream is full: only the reserve is free");
                      (void)rig->sink->advanceAnchor(streamP);
                      // Fill the ledger's own segment while the medium is full for producers: it opens a segment from the reserve.
                      for (int k = 0; k < 12 && segmentsOf(rig->medium, "ledger/1").size() < 2; ++k) {
                          const std::string id = "filler/" + std::to_string(k);
                          (void)rig->sink->accept(makeEvent(id, 1));
                          (void)rig->sink->closeStream(id);
                      }
                      checks.expect(segmentsOf(rig->medium, "ledger/1").size() >= 2, "the ledger opened a second segment");
                      checks.expect(rig->sink->health().freeSegmentsBeyondReserve == 0, "from the reserve: producers still have none");
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Trimmed && result.segmentsReclaimed >= 1,
                                    "the trim record was written and segments were reclaimed");
                      checks.expect(rig->sink->health().freeSegmentsBeyondReserve >= 1, "a free segment beyond the reserve exists again");
                      checks.expect(rig->sink->accept(makeEvent(streamP, fed + 1, big)), "the stream leaves the full state with the next event");
                      std::uint64_t more = fed + 1;
                      while (more < fed + 6 && rig->sink->accept(makeEvent(streamP, more + 1, big)))
                          ++more;
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Anchored && stream->report.lastPresent == more
                                        && stream->report.firstRetained == result.trimmedThrough + 1,
                                    "one chain from q + 1 to the last record: nothing skipped, nothing written twice");
                      checks.raise();
                  })
            .Then("relieve() frees segments only under the declared policy, and never without it",
                  [] {
                      speclab::core::Checks checks;
                      const auto            fill = [](Rig& rig) {
                          std::uint64_t fed = 0;
                          while (rig.sink->accept(makeEvent(streamP, fed + 1, big)))
                              ++fed;
                          (void)rig.sink->advanceAnchor(streamP);
                          return fed;
                      };
                      auto without = makeRig();
                      checks.expect(without->start(), "started with no retention policy");
                      const auto fedWithout = fill(*without);
                      checks.expect(without->sink->relieve() == 0 && trimsOf(*without, "ledger/1") == 0, "nothing is removed without a declared policy");
                      checks.expect(!without->sink->accept(makeEvent(streamP, fedWithout + 1, big)), "the stream stays full until the host acts");
                      auto with               = makeRig();
                      auto policy             = with->config();
                      policy.retention.rotate = true;
                      checks.expect(with->start(policy), "started with rotation declared");
                      const auto fedWith = fill(*with);
                      const auto freed   = with->sink->relieve();
                      checks.expect(freed >= 1 && trimsOf(*with, "ledger/1") == 1, "a trim was recorded and segments were reclaimed");
                      checks.expect(with->sink->accept(makeEvent(streamP, fedWith + 1, big)), "the full stream continues");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register interruptions{
    "An interruption after the trim is confirmed leaves a state the reader recognises, and the next trim completes it",
    "integration",
    [] {
        return speclab::Test("audit-rotation-interrupted")
            .Then("power lost before any segment is reclaimed: the removal did not happen, the stream is verified from H_0 and H_q is checked too",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      feedAnchored(*rig, 3 * segFrames, (4 * segFrames) + 2);
                      rig->medium.inject({.operation = Operation::Reclaim, .ordinal = 1, .effect = Effect::CutBefore});
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::ReclaimInterrupted && result.segmentsReclaimed == 0,
                                    "interrupted before the first reclaim");
                      rig->powerLoss();
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::RemovalNotCarriedOut
                                        && stream->report.verdict == Verdict::Anchored && stream->report.firstRetained == 1,
                                    "every record is there, verified from H_0");
                      checks.expect(report.has(BoundaryKind::RemovalNotCarriedOut, streamP), "the trim is reported as recorded and not carried out");
                      checks.expect(rig->start(), "restarted");
                      const auto again = rig->sink->trimPrefix(streamP);
                      checks.expect(again.outcome == RetentionOutcome::Trimmed && again.segmentsReclaimed == 3, "the next trim completes the removal");
                      const auto done = readLog(*rig);
                      checks.expect(reportOf(done, streamP)->disposition == StreamDisposition::Rotated
                                        && reportOf(done, streamP)->report.verdict == Verdict::Anchored,
                                    "the stream is rotated and anchored");
                      checks.raise();
                  })
            .Then("power lost part way: the leftover records reach the trim's digest and are reported as left over",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      feedAnchored(*rig, 3 * segFrames, (4 * segFrames) + 2);
                      rig->medium.inject({.operation = Operation::Reclaim, .ordinal = 2, .effect = Effect::CutBefore});
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::ReclaimInterrupted && result.segmentsReclaimed == 1, "one segment went");
                      rig->powerLoss();
                      const auto held = heldOf(rig->medium, streamP);
                      checks.expect(held.first == segFrames + 1, "records k … remain with k above 1");
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::InterruptedRemoval
                                        && stream->report.verdict == Verdict::Anchored && stream->report.firstRetained == (3 * segFrames) + 1
                                        && stream->report.anchoredThrough == 3 * segFrames,
                                    "the leftovers reach H_q, and the stream is verified from q + 1");
                      const auto left = report.notesOf(BoundaryKind::LeftoverFromInterruptedRemoval);
                      checks.expect(left.size() == 1 && left[0].position == segFrames + 1 && left[0].secondPosition == 3 * segFrames,
                                    "records k … q are reported as left over");
                      checks.expect(rig->start(), "restarted");
                      checks.expect(rig->sink->restart().faults.empty(), "recovery treats it as consistent: no fault");
                      checks.raise();
                  })
            .Then("a reclaim that answers failed leaves the same state without a power loss",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      feedAnchored(*rig, 3 * segFrames, (4 * segFrames) + 2);
                      rig->medium.inject({.operation = Operation::Reclaim, .ordinal = 3, .effect = Effect::Fail});
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::ReclaimInterrupted && result.segmentsReclaimed == 2, "two went, the third failed");
                      checks.expect(reportOf(readLog(*rig), streamP)->disposition == StreamDisposition::InterruptedRemoval, "interrupted");
                      checks.raise();
                  })
            .Execute();
    }};


/** @brief A session on a fresh log: P gets `count` records, is closed in order, and the session is left running (its sink is the rig's). */
void closedStream(Rig& rig, std::uint64_t count, std::size_t detailSize = 12) {
    (void)rig.feed(streamP, 1, count, detailSize);
    (void)rig.sink->closeStream(streamP);
}

void forgeLedger(StorageMedium& medium, std::string_view id, std::initializer_list<LedgerEntry> entries) {
    std::vector<AuditEvent> events;
    std::uint64_t           sequence = 0;
    for (const LedgerEntry& entry : entries)
        events.push_back(ledgerEvent(entry, id, ++sequence));
    (void)forgeSegment(medium, id, events);
}

[[nodiscard]] std::vector<AuditEvent> tenEvents() {
    std::vector<AuditEvent> events;
    for (std::uint64_t k = 1; k <= 10; ++k)
        events.push_back(makeEvent(streamP, k));
    return events;
}

const speclab::Register wholeStream{
    "A whole ended stream is removed after its trim, and the provider is told only when it holds an anchor",
    "integration",
    [] {
        return speclab::Test("audit-retention-whole-stream")
            .Then("an anchored stream: trim, removal, then retire; the reader finds it Retired and the trim's digest matches the retirement's",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      closedStream(*rig, 5);
                      const auto result = rig->sink->removeStream(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Removed && result.trimmedThrough == 5 && result.segmentsReclaimed == 1
                                        && result.retired,
                                    "removed and retired");
                      checks.expect(heldOf(rig->medium, streamP).count == 0, "no record is left");
                      checks.expect(std::holds_alternative<Retirement>(rig->provider.inner.latest(streamP)), "the provider holds a retirement");
                      RetainedPosition retained;
                      const auto       report = readLog(*rig, retained);
                      const auto*      stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::Removed && stream->report.verdict == Verdict::Retired,
                                    "Retired");
                      checks.expect(report.notesOf(BoundaryKind::RemovedWithoutAnchor).empty(), "q equals the retirement's position: nothing was unanchored");
                      checks.expect(retained.anchor(streamP).has_value() && retained.anchor(streamP)->retired, "the reader retains the retirement");
                      checks.raise();
                  })
            .Then("records past the last anchor were never anchored, and the trim record is the only trace of them",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      (void)rig->feed(streamP, 1, 5);
                      (void)rig->sink->advanceAnchor(streamP);
                      rig->provider.inner.setAvailable(false);
                      (void)rig->feed(streamP, 6, 3);
                      checks.expect(rig->sink->closeStream(streamP), "an orderly close while the provider is down");
                      checks.expect(rig->sink->health().counters.anchorsUnavailable >= 1, "the anchor attempt was counted as unavailable");
                      rig->provider.inner.setAvailable(true);
                      const auto result = rig->sink->removeStream(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Removed && result.trimmedThrough == 8 && result.retired,
                                    "removed through 8, retired at 5");
                      const auto report  = readLog(*rig);
                      const auto without = report.notesOf(BoundaryKind::RemovedWithoutAnchor);
                      checks.expect(without.size() == 1 && without[0].position == 6 && without[0].secondPosition == 8,
                                    "records 6 … 8 are reported as removed without anchor");
                      checks.expect(reportOf(report, streamP)->report.verdict == Verdict::Retired, "still Retired: the retirement is the anchor");
                      checks.raise();
                  })
            .Then("a stream the provider never anchored needs no retirement, and the trim record alone states the removal",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->provider.inner.setAvailable(false);
                      checks.expect(rig->start(), "started while the provider is down");
                      closedStream(*rig, 4);
                      rig->provider.inner.setAvailable(true);
                      const auto result = rig->sink->removeStream(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Removed && !result.retired && rig->provider.retires == 0,
                                    "removed, and retire was never called");
                      const auto report = readLog(*rig);
                      checks.expect(report.has(BoundaryKind::RemovedNeverAnchored, streamP), "removed under retention, never anchored");
                      checks.expect(reportOf(report, streamP)->report.verdict == Verdict::Unanchored, "with no finding");
                      checks.raise();
                  })
            .Then("a provider that does not answer: nothing is written and nothing is removed, and retention waits",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      closedStream(*rig, 4);
                      rig->provider.inner.setAvailable(false);
                      const auto result = rig->sink->removeStream(streamP);
                      checks.expect(result.outcome == RetentionOutcome::ProviderUnavailable, "waits rather than guessing between the two cases");
                      checks.expect(trimsOf(*rig, "ledger/1") == 0 && heldOf(rig->medium, streamP).count == 4, "no trim, no removal");
                      rig->provider.inner.setAvailable(true);
                      checks.expect(rig->sink->removeStream(streamP).outcome == RetentionOutcome::Removed, "and it completes when the provider answers");
                      checks.raise();
                  })
            .Then("an interruption between the removal and the retirement leaves the stream anchored with no records, and the next start completes the "
                  "retirement",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      closedStream(*rig, 5);
                      rig->provider.failRetire = true;
                      const auto result        = rig->sink->removeStream(streamP);
                      checks.expect(result.outcome == RetentionOutcome::Removed && !result.retired, "removed, retirement not accepted");
                      const auto  between = readLog(*rig);
                      const auto* stream  = reportOf(between, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Incomplete
                                        && stream->report.cause == VerdictCause::LogEndsBeforeAnchor && stream->report.lastPresent == 0,
                                    "Incomplete with m = 0");
                      checks.expect(between.has(BoundaryKind::RemovedUnderRetention, streamP), "the report cites the trim");
                      rig->provider.failRetire = false;
                      checks.expect(rig->start(), "restarted");
                      checks.expect(rig->sink->restart().retirementsCompleted == 1 && rig->sink->health().counters.retirements == 1,
                                    "the owed retirement was completed");
                      checks.expect(reportOf(readLog(*rig), streamP)->report.verdict == Verdict::Retired, "Retired");
                      checks.raise();
                  })
            .Then("a retirement is relayed only on the word of a ledger an anchor covers",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      forgeLedger(
                          rig->medium,
                          "ledger/1",
                          {LedgerEntry::origin("ledger/1"), LedgerEntry::streamOpen(streamP), LedgerEntry::streamTrim(streamP, 5, chainDigestAt(streamP, 5))});
                      checks.expect(std::holds_alternative<AnchorStamp>(rig->provider.advance(makeAnchorClaim(streamP, 5, chainDigestAt(streamP, 5)))),
                                    "the provider holds P");
                      rig->session = 1;
                      checks.expect(rig->start(), "started");
                      checks.expect(rig->sink->restart().retirementsCompleted == 0 && rig->provider.retires == 0, "no retirement was relayed");
                      checks.expect(hasFaultOf(rig->sink->restart().faults, IntegrityFaultKind::RetirementNotAuthorized, streamP), "and it is reported");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register refusals{
    "Retention refuses what it must not touch: a live stream, an inconsistent one, and anything on a medium that cannot confirm",
    "integration",
    [] {
        return speclab::Test("audit-retention-refusals")
            .Then("a stream still being written is not removed",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      (void)rig->feed(streamP, 1, 3);
                      checks.expect(rig->sink->removeStream(streamP).outcome == RetentionOutcome::StreamNotEnded, "live");
                      checks.expect(rig->sink->removeStream("unknown/stream").outcome == RetentionOutcome::UnknownStream, "unknown");
                      checks.expect(rig->sink->removeStream("ledger/1").outcome == RetentionOutcome::NotRotatable
                                        && rig->sink->trimPrefix("ledger/1").outcome == RetentionOutcome::NotRotatable,
                                    "the current ledger is never removed or rotated");
                      checks.raise();
                  })
            .Then("a stream recovery found inconsistent is never removed, and the evidence is not touched",
                  [] {
                      speclab::core::Checks   checks;
                      auto                    rig    = makeRig();
                      std::vector<AuditEvent> events = tenEvents();
                      forgeLedger(rig->medium,
                                  "ledger/1",
                                  {LedgerEntry::origin("ledger/1"),
                                   LedgerEntry::streamOpen(streamP),
                                   LedgerEntry::streamClose(streamP, 10, chainDigestAt(streamP, 10))});
                      const auto segment = forgeSegment(rig->medium, streamP, events, 0, 0, std::size_t{4});
                      const auto before  = rig->medium.bytesOf(segment);
                      rig->session       = 1;
                      checks.expect(rig->start(), "started");
                      checks.expect(rig->sink->removeStream(streamP).outcome == RetentionOutcome::StreamInconsistent, "refused");
                      checks.expect(rig->sink->trimPrefix(streamP).outcome == RetentionOutcome::StreamInconsistent, "and not rotated either");
                      checks.expect(rig->medium.bytesOf(segment) == before && trimsOf(*rig, "ledger/2") == 0, "nothing was written, nothing removed");
                      checks.expect(rig->sink->health().counters.retentionRefused == 2, "both refusals are counted");
                      checks.raise();
                  })
            .Then("a medium that cannot confirm the trim never loses a segment on its account",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig(24, false);
                      checks.expect(rig->start(rig->config(), false), "started on a medium that answers Unsupported");
                      const std::uint64_t segFrames = perSegment();
                      (void)rig->feed(streamP, 1, (2 * segFrames) + 1, big);
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::NotConfirmed && result.segmentsReclaimed == 0, "not confirmed, nothing reclaimed");
                      checks.expect(segmentsOf(rig->medium, streamP).size() == 3 && heldOf(rig->medium, streamP).first == 1, "every segment is still there");
                      checks.expect(rig->sink->closeStream(streamP) && rig->sink->removeStream(streamP).outcome == RetentionOutcome::NotConfirmed,
                                    "so is the removal of a whole stream");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register ledgerRetention{
    "A ledger is removed only when every stream it opened has gone and a newer ledger cites it",
    "integration",
    [] {
        return speclab::Test("audit-retention-ledger")
            .Then("the conditions of 10.5, then the retirement the next ledger's citation is checked against",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      closedStream(*rig, 5);
                      rig->sink->close();
                      checks.expect(rig->start(), "restarted");
                      checks.expect(rig->sink->removeStream("ledger/1").outcome == RetentionOutcome::LedgerStillNeeded, "a stream it opened is still held");
                      checks.expect(rig->sink->removeStream(streamP).outcome == RetentionOutcome::Removed, "the stream goes");
                      const auto result = rig->sink->removeStream("ledger/1");
                      checks.expect(result.outcome == RetentionOutcome::Removed && result.retired, "now the ledger goes, and its anchor is retired");
                      checks.expect(!rig->medium.segments()->empty() && segmentsOf(rig->medium, "ledger/1").empty(), "ledger/1 is gone from the medium");
                      const auto report = readLog(*rig);
                      checks.expect(reportOf(report, "ledger/1")->report.verdict == Verdict::Retired, "Retired");
                      checks.expect(report.has(BoundaryKind::PredecessorMatched, "ledger/2") && report.has(BoundaryKind::RecoveredMatched, "ledger/2"),
                                    "the citations are checked against the provider's retirements");
                      checks.expect(!report.has(BoundaryKind::PredecessorNotCheckable, "ledger/2"), "not 'no longer checkable'");
                      checks.raise();
                  })
            .Then("without a provider the link to a removed ledger is reported as no longer checkable",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(rig->config(), false), "started with no provider");
                      closedStream(*rig, 5);
                      rig->sink->close();
                      checks.expect(rig->start(rig->config(), false), "restarted");
                      checks.expect(rig->sink->removeStream(streamP).outcome == RetentionOutcome::Removed, "the stream goes");
                      checks.expect(rig->sink->removeStream("ledger/1").outcome == RetentionOutcome::Removed, "and the ledger");
                      const auto report = readLog(*rig);
                      checks.expect(report.has(BoundaryKind::PredecessorNotCheckable, "ledger/2")
                                        && report.has(BoundaryKind::RecoveredNotCheckable, "ledger/2"),
                                    "no longer checkable");
                      checks.expect(reportOf(report, streamP)->report.verdict == Verdict::Unanchored && report.has(BoundaryKind::RemovedNeverAnchored, streamP),
                                    "removed under retention, never anchored");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register missingRecords{
    "Records missing at the start of a stream, or after its trim, make it Incomplete and are never accounted for by silence",
    "unit",
    [] {
        return speclab::Test("audit-retention-missing")
            .Then("a prefix that is gone with no trim: Incomplete, and no anchor can be checked",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      (void)rig->feed(streamP, 1, (2 * segFrames) + 1, big);
                      (void)rig->sink->closeStream(streamP);
                      checks.expect(rig->medium.reclaim(segmentsOf(rig->medium, streamP).front().ref), "someone removed the first segment");
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::PrefixMissing
                                        && stream->report.verdict == Verdict::Incomplete && stream->report.cause == VerdictCause::PrefixMissingWithoutTrim
                                        && !stream->report.anchoredThrough,
                                    "Incomplete: the chain cannot start, so nothing is Anchored");
                      const auto notes = report.notesOf(BoundaryKind::PrefixMissingWithoutTrim);
                      checks.expect(notes.size() == 1 && notes[0].position == segFrames + 1, "records 1 … k - 1 are missing");
                      checks.expect(rig->start(), "restarted");
                      checks.expect(hasFaultOf(rig->sink->restart().faults, IntegrityFaultKind::RecordsMissing, streamP)
                                        && rig->sink->restart().recoveredFailed == 1,
                                    "recovery reports it and cites the stream as not checking");
                      checks.raise();
                  })
            .Then("records between a trim and the first record present are missing",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig    = makeRig();
                      const auto            events = tenEvents();
                      forgeLedger(
                          rig->medium,
                          "ledger/1",
                          {LedgerEntry::origin("ledger/1"), LedgerEntry::streamOpen(streamP), LedgerEntry::streamTrim(streamP, 3, chainDigestAt(streamP, 3))});
                      (void)forgeSegment(rig->medium, streamP, events, 5, 1);
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::GapAfterTrim && stream->report.verdict == Verdict::Incomplete
                                        && stream->report.cause == VerdictCause::RecordsMissingAfterTrim,
                                    "Incomplete");
                      const auto notes = report.notesOf(BoundaryKind::RecordsMissingAfterTrim);
                      checks.expect(notes.size() == 1 && notes[0].position == 4 && notes[0].secondPosition == 5, "records 4 … 5 are missing");
                      checks.raise();
                  })
            .Then("a trim that cites records that are gone, while the prefix is still there",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig    = makeRig();
                      const auto            events = tenEvents();
                      forgeLedger(
                          rig->medium,
                          "ledger/1",
                          {LedgerEntry::origin("ledger/1"), LedgerEntry::streamOpen(streamP), LedgerEntry::streamTrim(streamP, 12, chainDigestAt(streamP, 3))});
                      (void)forgeSegment(rig->medium, streamP, events);
                      const auto  streamLog = readLog(*rig);
                      const auto* stream    = reportOf(streamLog, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Incomplete
                                        && stream->report.cause == VerdictCause::TrimExceedsRecords,
                                    "Incomplete: records up to 12 were confirmed and only 10 are there");
                      checks.raise();
                  })
            .Then("a trim digest the records do not reproduce, with every record still present: Inconsistent",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig    = makeRig();
                      const auto            events = tenEvents();
                      forgeLedger(
                          rig->medium,
                          "ledger/1",
                          {LedgerEntry::origin("ledger/1"), LedgerEntry::streamOpen(streamP), LedgerEntry::streamTrim(streamP, 6, chainDigestAt(streamP, 5))});
                      (void)forgeSegment(rig->medium, streamP, events);
                      const auto  streamLog = readLog(*rig);
                      const auto* stream    = reportOf(streamLog, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::RemovalNotCarriedOut
                                        && stream->report.verdict == Verdict::Inconsistent && stream->report.cause == VerdictCause::TrimDigestMismatch,
                                    "the trim's H_q is checked too");
                      checks.raise();
                  })
            .Then("leftovers that reach the trim's digest are fine; leftovers that do not are Inconsistent",
                  [] {
                      speclab::core::Checks checks;
                      const auto            events = tenEvents();
                      for (const bool right : {true, false}) {
                          auto rig = makeRig();
                          forgeLedger(rig->medium,
                                      "ledger/1",
                                      {LedgerEntry::origin("ledger/1"),
                                       LedgerEntry::streamOpen(streamP),
                                       LedgerEntry::streamTrim(streamP, 6, chainDigestAt(streamP, right ? 6 : 5))});
                          (void)forgeSegment(rig->medium, streamP, events, 3, 1, std::nullopt, 6);
                          (void)forgeSegment(rig->medium, streamP, events, 6, 2);
                          const auto  streamLog = readLog(*rig);
                          const auto* stream    = reportOf(streamLog, streamP);
                          if (right) {
                              checks.expect(stream != nullptr && stream->disposition == StreamDisposition::InterruptedRemoval
                                                && stream->report.verdict == Verdict::Unanchored && stream->report.firstRetained == 7
                                                && stream->report.lastPresent == 10,
                                            "left over from an interrupted removal, verified from q + 1");
                          } else {
                              checks.expect(stream != nullptr && stream->report.verdict == Verdict::Inconsistent
                                                && stream->report.cause == VerdictCause::LeftoverDoesNotReachTrim,
                                            "the leftovers do not reach H_q");
                          }
                      }
                      checks.raise();
                  })
            .Then("a trim inside a segment is never written by the adapter, and a reader finds it Inconsistent",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig    = makeRig();
                      const auto            events = tenEvents();
                      forgeLedger(
                          rig->medium,
                          "ledger/1",
                          {LedgerEntry::origin("ledger/1"), LedgerEntry::streamOpen(streamP), LedgerEntry::streamTrim(streamP, 6, chainDigestAt(streamP, 6))});
                      (void)forgeSegment(rig->medium, streamP, events, 3, 1);
                      const auto  streamLog = readLog(*rig);
                      const auto* stream    = reportOf(streamLog, streamP);
                      checks.expect(stream != nullptr && stream->disposition == StreamDisposition::TrimNotOnSegmentBoundary
                                        && stream->report.verdict == Verdict::Inconsistent && stream->report.cause == VerdictCause::TrimNotOnSegmentBoundary,
                                    "Inconsistent");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register reviewCases{
    "A ledger left over by an interrupted removal is accounted for by its trim, and a retirement needs a trim that covers the anchor",
    "integration",
    [] {
        return speclab::Test("audit-retention-review-cases")
            .Then("what an interrupted removal left of an earlier ledger is not a stream no ledger opened, and the next removal completes it",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig(40);
                      checks.expect(rig->start(), "started");
                      std::vector<std::string> fillers;
                      for (int k = 0; k < 14; ++k) {
                          fillers.push_back("filler/" + std::to_string(k));
                          (void)rig->sink->accept(makeEvent(fillers.back(), 1));
                          (void)rig->sink->closeStream(fillers.back());
                      }
                      rig->sink->close();
                      checks.expect(segmentsOf(rig->medium, "ledger/1").size() >= 2, "the first ledger spans two segments");
                      checks.expect(rig->start(), "restarted");
                      for (const auto& id : fillers) {
                          if (heldOf(rig->medium, id).count != 0)  // a filler the full medium refused stored nothing
                              checks.expect(rig->sink->removeStream(id).outcome == RetentionOutcome::Removed, "a stream goes");
                      }
                      rig->medium.inject({.operation = Operation::Reclaim, .ordinal = rig->medium.calls(Operation::Reclaim) + 2, .effect = Effect::CutBefore});
                      const auto cut = rig->sink->removeStream("ledger/1");
                      checks.expect(cut.outcome == RetentionOutcome::ReclaimInterrupted && cut.segmentsReclaimed == 1,
                                    "the removal of the ledger is interrupted");
                      rig->powerLoss();
                      checks.expect(rig->start(), "restarted again");
                      checks.expect(!std::ranges::any_of(rig->sink->restart().faults,
                                                         [](const IntegrityFault& fault) {
                                                             return fault.kind == IntegrityFaultKind::StreamNotOpened;
                                                         }),
                                    "the leftover is not reported as a stream no ledger opened");
                      checks.expect(rig->sink->removeStream("ledger/1").outcome == RetentionOutcome::Removed, "the next removal completes it");
                      checks.expect(segmentsOf(rig->medium, "ledger/1").empty(), "nothing of ledger/1 is left");
                      checks.raise();
                  })
            .Then("only the newest origin ledger claims an origin while earlier history exists",
                  [] {
                      speclab::core::Checks checks;
                      auto                  before = makeRig();
                      (void)before->start();
                      closedStream(*before, 3);
                      before->sink->close();
                      auto after = makeRig();
                      after->provider.inner.restore(before->provider.inner.snapshot());
                      after->session = 5;
                      checks.expect(after->start(), "an origin ledger on a wiped log");
                      checks.expect(after->start(), "and a second ledger that cites it");
                      const auto report = readLog(*after);
                      checks.expect(report.has(BoundaryKind::NoEarlierHistoryKnown, "ledger/6")
                                        && !report.has(BoundaryKind::OriginClaimedWhileHistoryExists, "ledger/6"),
                                    "the first ledger of this log is not blamed for history it never claimed");
                      checks.raise();
                  })
            .Then("a trim that stops short of the anchor is not relayed as a retirement",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      forgeLedger(
                          rig->medium,
                          "ledger/1",
                          {LedgerEntry::origin("ledger/1"), LedgerEntry::streamOpen(streamP), LedgerEntry::streamTrim(streamP, 3, chainDigestAt(streamP, 3))});
                      (void)rig->provider.advance(makeAnchorClaim(streamP, 5, chainDigestAt(streamP, 5)));
                      const LogAnalysis log = LogAnalysis::read(rig->medium);
                      (void)rig->provider.advance(makeAnchorClaim("ledger/1", 3, log.ledger("ledger/1")->headDigest));
                      rig->session = 1;
                      checks.expect(rig->start(), "started");
                      checks.expect(rig->provider.retires == 0 && rig->sink->restart().retirementsCompleted == 0, "no retirement was relayed");
                      checks.expect(hasFaultOf(rig->sink->restart().faults, IntegrityFaultKind::AnchoredEvidenceGone, streamP),
                                    "the anchored evidence is reported as gone");
                      checks.raise();
                  })
            .Execute();
    }};

/** @brief One cut point of a retention operation. */
struct Cut {
    Operation   operation;
    std::size_t ordinal;
    Effect      effect;
    std::size_t partialBytes = 0;
};

const speclab::Register retentionCuts{
    "Power lost at any write or acknowledgment point of a rotation or a removal leaves a state the reader recognises and the next start completes",
    "integration",
    [] {
        return speclab::Test("audit-retention-cuts")
            .Then("rotation, cut at every point of the trim record and of the removal",
                  [] {
                      speclab::core::Checks checks;
                      const std::uint64_t   segFrames = perSegment();
                      const auto            prepare   = [&] {
                          auto rig = makeRig();
                          (void)rig->start();
                          feedAnchored(*rig, 3 * segFrames, (4 * segFrames) + 2);
                          return rig;
                      };
                      auto       dry           = prepare();
                      const auto openBefore    = dry->medium.calls(Operation::Open);
                      const auto appendBefore  = dry->medium.calls(Operation::Append);
                      const auto syncBefore    = dry->medium.calls(Operation::Sync);
                      const auto reclaimBefore = dry->medium.calls(Operation::Reclaim);
                      checks.expect(dry->sink->trimPrefix(streamP).outcome == RetentionOutcome::Trimmed, "the dry run trims");
                      std::vector<Cut> cuts;
                      for (auto at = appendBefore + 1; at <= dry->medium.calls(Operation::Append); ++at) {
                          cuts.push_back({Operation::Append, at, Effect::CutBefore, 0});
                          cuts.push_back({Operation::Append, at, Effect::CutPartial, 20});
                          cuts.push_back({Operation::Append, at, Effect::CutAfter, 0});
                      }
                      for (auto at = syncBefore + 1; at <= dry->medium.calls(Operation::Sync); ++at) {
                          cuts.push_back({Operation::Sync, at, Effect::CutBefore, 0});
                          cuts.push_back({Operation::Sync, at, Effect::CutAfter, 0});
                      }
                      for (auto at = reclaimBefore + 1; at <= dry->medium.calls(Operation::Reclaim); ++at) {
                          cuts.push_back({Operation::Reclaim, at, Effect::CutBefore, 0});
                          cuts.push_back({Operation::Reclaim, at, Effect::CutAfter, 0});
                      }
                      (void)openBefore;
                      checks.expect(cuts.size() >= 10, "the operation has several write and acknowledgment points");
                      bool recognised = true;
                      bool completes  = true;
                      for (const Cut& cut : cuts) {
                          auto rig = prepare();
                          rig->medium.inject({.operation = cut.operation, .ordinal = cut.ordinal, .effect = cut.effect, .partialBytes = cut.partialBytes});
                          (void)rig->sink->trimPrefix(streamP);
                          rig->powerLoss();
                          const auto  first  = readLog(*rig);
                          const auto* stream = reportOf(first, streamP);
                          recognised         = recognised && stream != nullptr && stream->report.verdict == Verdict::Anchored
                                       && stream->report.anchoredThrough == 3 * segFrames;
                          recognised = recognised && stream->disposition != StreamDisposition::GapAfterTrim
                                       && stream->disposition != StreamDisposition::PrefixMissing;
                          recognised          = recognised && std::ranges::none_of(first.streams, [](const StreamBoundaryReport& item) {
                                           return item.report.verdict == Verdict::Inconsistent;
                                       });
                          completes           = completes && rig->start();
                          const auto again    = rig->sink->trimPrefix(streamP);
                          completes           = completes && (again.outcome == RetentionOutcome::Trimmed || again.outcome == RetentionOutcome::NothingToTrim);
                          const auto  doneLog = readLog(*rig);
                          const auto* done    = reportOf(doneLog, streamP);
                          completes           = completes && done != nullptr && done->disposition == StreamDisposition::Rotated
                                      && done->report.verdict == Verdict::Anchored;
                      }
                      checks.expect(recognised,
                                    "after any cut the stream is Anchored through p, in a disposition the reader names, and nothing is Inconsistent");
                      checks.expect(completes, "the next start's trim completes the rotation");
                      checks.raise();
                  })
            .Then("whole-stream removal, cut at every point, ends Retired once the next start has completed the retirement",
                  [] {
                      speclab::core::Checks checks;
                      const auto            prepare = [] {
                          auto rig = makeRig();
                          (void)rig->start();
                          closedStream(*rig, 5);
                          return rig;
                      };
                      auto       dry           = prepare();
                      const auto appendBefore  = dry->medium.calls(Operation::Append);
                      const auto syncBefore    = dry->medium.calls(Operation::Sync);
                      const auto reclaimBefore = dry->medium.calls(Operation::Reclaim);
                      checks.expect(dry->sink->removeStream(streamP).outcome == RetentionOutcome::Removed, "the dry run removes");
                      std::vector<Cut> cuts;
                      for (auto at = appendBefore + 1; at <= dry->medium.calls(Operation::Append); ++at) {
                          cuts.push_back({Operation::Append, at, Effect::CutBefore, 0});
                          cuts.push_back({Operation::Append, at, Effect::CutPartial, 20});
                          cuts.push_back({Operation::Append, at, Effect::CutAfter, 0});
                      }
                      for (auto at = syncBefore + 1; at <= dry->medium.calls(Operation::Sync); ++at) {
                          cuts.push_back({Operation::Sync, at, Effect::CutBefore, 0});
                          cuts.push_back({Operation::Sync, at, Effect::CutAfter, 0});
                      }
                      for (auto at = reclaimBefore + 1; at <= dry->medium.calls(Operation::Reclaim); ++at) {
                          cuts.push_back({Operation::Reclaim, at, Effect::CutBefore, 0});
                          cuts.push_back({Operation::Reclaim, at, Effect::CutAfter, 0});
                      }
                      bool noFinding = true;
                      bool settles   = true;
                      for (const Cut& cut : cuts) {
                          auto rig = prepare();
                          rig->medium.inject({.operation = cut.operation, .ordinal = cut.ordinal, .effect = cut.effect, .partialBytes = cut.partialBytes});
                          (void)rig->sink->removeStream(streamP);
                          rig->powerLoss();
                          const auto first = readLog(*rig);
                          for (const auto& item : first.streams) {
                              const auto verdict = item.report.verdict;
                              noFinding = noFinding && verdict != Verdict::Inconsistent && verdict != Verdict::Altered && verdict != Verdict::RolledBack
                                          && verdict != Verdict::Conflict && verdict != Verdict::CannotVerify;
                          }
                          settles             = settles && rig->start();
                          const auto  again   = rig->sink->removeStream(streamP);
                          const auto  doneLog = readLog(*rig);
                          const auto* done    = reportOf(doneLog, streamP);
                          settles             = settles && done != nullptr && done->report.verdict == Verdict::Retired;
                          (void)again;
                      }
                      checks.expect(noFinding, "after any cut no stream is Inconsistent, Altered, rolled back or unverifiable");
                      checks.expect(settles, "the next start completes the removal and the retirement: Retired");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register removalRecords{
    "Retention never erases the only record of a removal, and never takes a rotation's trim for the removal of a whole stream",
    "integration",
    [] {
        return speclab::Test("audit-retention-removal-records")
            .Then("on a medium that answers Unsupported, no trim is written at all, so repeated attempts never make the ledger malformed",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig(24, false);
                      checks.expect(rig->start(rig->config(), false), "started on a medium that answers Unsupported");
                      (void)rig->feed(streamP, 1, (2 * perSegment()) + 1, big);
                      const auto first  = rig->sink->trimPrefix(streamP);
                      const auto second = rig->sink->trimPrefix(streamP);
                      checks.expect(first.outcome == RetentionOutcome::NotConfirmed && second.outcome == RetentionOutcome::NotConfirmed,
                                    "both attempts are refused as not confirmable");
                      checks.expect(first.trimmedThrough == 0 && second.trimmedThrough == 0 && trimsOf(*rig, "ledger/1") == 0, "and no trim was written");
                      const LogAnalysis log = LogAnalysis::read(rig->medium);
                      checks.expect(log.ledger("ledger/1") != nullptr && log.ledger("ledger/1")->wellFormed(), "the ledger stays well formed");
                      checks.raise();
                  })
            .Then("a trim appended whose sync fails is reported as recorded, and nothing is removed",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(rig->config(), false), "started with no provider");
                      (void)rig->feed(streamP, 1, (2 * perSegment()) + 1, big);
                      rig->medium.inject({.operation = Operation::Sync, .ordinal = rig->medium.calls(Operation::Sync) + 1, .effect = Effect::Fail});
                      const auto result = rig->sink->trimPrefix(streamP);
                      checks.expect(result.outcome == RetentionOutcome::NotConfirmed && result.segmentsReclaimed == 0, "not confirmed, nothing reclaimed");
                      checks.expect(result.trimmedThrough == 2 * perSegment() && trimsOf(*rig, "ledger/1") == 1,
                                    "the trim reached the medium, and the result says so");
                      checks.raise();
                  })
            .Then(
                "a ledger that holds the only trim of a stream still held is not removed, and goes once a newer trim covers it",
                [] {
                    speclab::core::Checks checks;
                    auto                  rig = makeRig();
                    checks.expect(rig->start(), "first start: P is opened by ledger/1");
                    const std::uint64_t segFrames = perSegment();
                    feedAnchored(*rig, 2 * segFrames, (3 * segFrames) + 1);
                    checks.expect(rig->sink->closeStream(streamP), "P closed and anchored at its end");
                    rig->sink->close();
                    checks.expect(rig->start(), "second start");
                    const auto rotated = rig->sink->trimPrefix(streamP);
                    checks.expect(rotated.outcome == RetentionOutcome::Trimmed, "ledger/2 records the trim of P, a stream it did not open");
                    rig->sink->close();
                    checks.expect(rig->start(), "third start");
                    checks.expect(rig->sink->removeStream("ledger/2").outcome == RetentionOutcome::LedgerStillNeeded,
                                  "ledger/2 holds the only trim of P, which the log still holds");
                    const auto  report = readLog(*rig);
                    const auto* stream = reportOf(report, streamP);
                    checks.expect(stream != nullptr && stream->disposition == StreamDisposition::Rotated, "P still reads as rotated, not as a missing prefix");
                    checks.expect(rig->sink->removeStream(streamP).outcome == RetentionOutcome::Removed, "P is removed as a whole");
                    checks.expect(rig->sink->removeStream("ledger/2").outcome == RetentionOutcome::Removed, "now ledger/3's trim covers it, and ledger/2 goes");
                    checks.expect(rig->sink->health().integrity.empty(), "no integrity fault on the way");
                    checks.raise();
                })
            .Then("a ledger that holds the trim authorizing an owed retirement is kept until the retirement is done",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "first start");
                      closedStream(*rig, 5);
                      rig->provider.failRetire = true;
                      const auto removed       = rig->sink->removeStream(streamP);
                      checks.expect(removed.outcome == RetentionOutcome::Removed && !removed.retired, "P is gone and its retirement is owed");
                      rig->sink->close();
                      checks.expect(rig->start(), "second start, the provider still refuses to retire");
                      rig->sink->close();
                      checks.expect(rig->start(), "third start");
                      checks.expect(rig->sink->removeStream("ledger/1").outcome == RetentionOutcome::LedgerStillNeeded,
                                    "ledger/1 holds the trim that authorizes the owed retirement");
                      rig->sink->close();
                      rig->provider.failRetire = false;
                      checks.expect(rig->start(), "fourth start completes the retirement");
                      checks.expect(rig->sink->restart().retirementsCompleted == 1, "retired");
                      checks.expect(rig->sink->removeStream("ledger/1").outcome == RetentionOutcome::Removed, "and only now ledger/1 goes");
                      checks.raise();
                  })
            .Then("a rotation's trim at the anchor, followed by the loss of the records after it, is a finding and never a retirement",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "first start");
                      const std::uint64_t segFrames = perSegment();
                      const std::uint64_t p         = 2 * segFrames;  // a segment end
                      const std::uint64_t m         = (3 * segFrames) + 1;
                      feedAnchored(*rig, p, m);
                      rig->powerLoss();
                      checks.expect(rig->start(), "second start: ledger/2 records P as recovered at m");
                      const auto rotated = rig->sink->trimPrefix(streamP);
                      checks.expect(rotated.outcome == RetentionOutcome::Trimmed && rotated.trimmedThrough == p, "a rotation with q = p");
                      for (const auto& segment : segmentsOf(rig->medium, streamP))
                          checks.expect(rig->medium.reclaim(segment.ref), "someone removes the records after the trim");
                      rig->sink->close();
                      checks.expect(rig->start(), "third start");
                      checks.expect(rig->sink->restart().retirementsCompleted == 0 && rig->provider.retires == 0, "no retirement is relayed");
                      checks.expect(hasFaultOf(rig->sink->restart().faults, IntegrityFaultKind::RecordsMissing, streamP), "the loss is an integrity fault");
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Incomplete
                                        && stream->report.cause == VerdictCause::RecordsMissingAfterTrim,
                                    "the reader says Incomplete, not Retired");
                      const auto missing = report.notesOf(BoundaryKind::RecordsMissingAfterTrim);
                      checks.expect(std::ranges::any_of(missing,
                                                        [&](const BoundaryNote& note) {
                                                            return note.stream == streamP && note.position == p + 1 && note.secondPosition == m;
                                                        }),
                                    "records q + 1 … m are reported missing");
                      checks.raise();
                  })
            .Then("a reader does not accept a retirement past the highest trim",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const std::uint64_t segFrames = perSegment();
                      const std::uint64_t m         = (3 * segFrames) + 1;
                      (void)rig->feed(streamP, 1, m, big);
                      checks.expect(rig->sink->closeStream(streamP), "closed and anchored at m");
                      const auto rotated = rig->sink->trimPrefix(streamP);
                      checks.expect(rotated.outcome == RetentionOutcome::Trimmed && rotated.trimmedThrough < m, "a rotation below m");
                      for (const auto& segment : segmentsOf(rig->medium, streamP))
                          (void)rig->medium.reclaim(segment.ref);
                      checks.expect(std::holds_alternative<AnchorStamp>(rig->provider.inner.retire(streamP, m)), "someone retires P at m");
                      const auto  report = readLog(*rig);
                      const auto* stream = reportOf(report, streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Incomplete
                                        && stream->report.cause == VerdictCause::RetirementBeyondTrim,
                                    "Incomplete: anchored records went with no trim");
                      checks.raise();
                  })
            .Then("relieve() reads the log and recovers chain state once while its attempts change nothing",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig    = makeRig();
                      auto                  policy = rig->config();
                      policy.retention.rotate      = true;
                      checks.expect(rig->start(policy), "started with rotation declared");
                      constexpr int fillers = 6;
                      for (int k = 0; k < fillers; ++k) {
                          const std::string id = "filler/" + std::to_string(k);
                          (void)rig->sink->accept(makeEvent(id, 1));
                          (void)rig->sink->closeStream(id);
                      }
                      std::uint64_t fed = 0;
                      while (rig->sink->accept(makeEvent(streamP, fed + 1, big)))
                          ++fed;
                      checks.expect(rig->sink->health().counters.fullEntries == 1, "P is full and has no anchor, so nothing can be rotated");
                      const std::size_t before = rig->provider.latests;
                      checks.expect(rig->sink->relieve() == 0, "nothing is freed");
                      const std::size_t held = fillers + 2;  // the fillers, P and the ledger
                      checks.expect(rig->provider.latests - before <= 2 * (held + 1), "one recovery for every attempt, not one per attempt");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
