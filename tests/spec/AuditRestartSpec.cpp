/** @brief ADR-004 Decision 10.2, 10.3 and 10.6: the ledger, restart, chain state recovery, and what a reader reports at each boundary (issue #92). */

import std;
import speclab;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

#include "../framework/AuditLogRig.hpp"

namespace {

using namespace mddlog::spec::auditlog;
using Operation   = InMemoryStorageMedium::Operation;
using Effect      = InMemoryStorageMedium::Effect;
using Unconfirmed = InMemoryStorageMedium::Unconfirmed;

constexpr std::string_view streamP = "device-42/boot-7";
constexpr std::string_view streamQ = "device-42/boot-8";
constexpr std::string_view streamR = "device-42/boot-9";

[[nodiscard]] const StreamStorageHealth* healthOf(const StorageHealthSnapshot& health, std::string_view id) {
    const auto found = std::ranges::find(health.streams, id, &StreamStorageHealth::streamId);
    return found == health.streams.end() ? nullptr : &*found;
}

[[nodiscard]] bool hasFault(std::span<const IntegrityFault> faults, IntegrityFaultKind kind, std::string_view stream) {
    return std::ranges::any_of(faults, [&](const IntegrityFault& fault) {
        return fault.kind == kind && fault.stream == stream;
    });
}

[[nodiscard]] std::vector<std::string> actions(std::initializer_list<std::string_view> lines) {
    std::vector<std::string> out;
    for (const auto line : lines)
        out.emplace_back(line);
    return out;
}

/** @brief A session that fed `count` events to P and closed in order. */
void orderlySession(Rig& rig, std::uint64_t count = 5) {
    (void)rig.start();
    (void)rig.feed(streamP, 1, count);
    rig.sink->close();
}

/** @brief The ledger of the log with the given identity, as a reader parsed it. */
struct Parsed {
    explicit Parsed(Rig& rig) : log(LogAnalysis::read(rig.medium)) {}
    LogAnalysis log;
};

const speclab::Register firstStart{
    "A first start writes origin, confirmed, and records each stream before any of its records",
    "integration",
    [] {
        return speclab::Test("audit-restart-first-start")
            .Then("an empty log begins its ledger with origin, and a reader says no earlier history is known rather than that the log is whole",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const auto& restart = rig->sink->restart();
                      checks.expect(restart.ledgerEnabled && restart.startWritten && restart.firstRecord == LedgerRecordKind::Origin
                                        && restart.predecessor.empty(),
                                    "origin as record 1");
                      checks.expect(ledgerActions(rig->medium) == actions({"ledger/1: mddlog.ledger.origin ledger/1"}),
                                    "the ledger holds origin and nothing else");
                      const auto  health = rig->sink->health();
                      const auto* mine   = healthOf(health, "ledger/1");
                      checks.expect(mine != nullptr && mine->durablePosition == 1 && mine->state == StreamStorageState::Persisting,
                                    "record 1 is durably confirmed");
                      const auto  report = readLog(*rig);
                      const auto* found  = report.find("ledger/1");
                      checks.expect(found != nullptr && found->ledger && found->report.verdict == Verdict::Unanchored,
                                    "the ledger is internally consistent, unanchored");
                      checks.expect(report.has(BoundaryKind::NoEarlierHistoryKnown, "ledger/1"), "no earlier history known");
                      checks.expect(report.has(BoundaryKind::LedgerNotClosed, "ledger/1"), "and no close recorded");
                      checks.raise();
                  })
            .Then("the open record is durably confirmed before the stream's first record is stored",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      const auto appendsAfterStart = rig->medium.calls(Operation::Append);
                      // Appends from here: the ledger's open record, then the producer's first frame. Power is lost before the second.
                      rig->medium.inject({.operation = Operation::Append, .ordinal = appendsAfterStart + 2, .effect = Effect::CutBefore});
                      (void)rig->sink->accept(makeEvent(streamP, 1));
                      checks.expect(rig->medium.poweredOff(), "power was lost at the producer's first append");
                      rig->powerLoss();
                      checks.expect(ledgerActions(rig->medium)
                                        == actions({"ledger/1: mddlog.ledger.origin ledger/1", "ledger/1: mddlog.stream.open device-42/boot-7"}),
                                    "the open record survived: a boundary is recorded before it happens");
                      checks.expect(heldOf(rig->medium, streamP).count == 0, "and no record of the stream was stored");
                      checks.raise();
                  })
            .Then("a medium that cannot confirm stores ledger records and never confirms them",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig(24, false);
                      checks.expect(rig->start(), "started");
                      (void)rig->feed(streamP, 1, 2);
                      const auto  health = rig->sink->health();
                      const auto* mine   = healthOf(health, "ledger/1");
                      checks.expect(mine != nullptr && mine->durablePosition == 0 && mine->appendedPosition == 2
                                        && mine->state == StreamStorageState::Persisting,
                                    "appended, never confirmed, and not a failure");
                      checks.expect(rig->provider.advances == 0, "nothing is anchored without a confirmation");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register orderlyRestart{
    "A restart after an orderly close links the new ledger to the old one and starts new stream instances",
    "integration",
    [] {
        return speclab::Test("audit-restart-orderly")
            .Then("close writes each stream's close after its anchor attempt, then ledger.close, and anchors the ledger",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      orderlySession(*rig);
                      checks.expect(ledgerActions(rig->medium)
                                        == actions({"ledger/1: mddlog.ledger.origin ledger/1",
                                                    "ledger/1: mddlog.stream.open device-42/boot-7",
                                                    "ledger/1: mddlog.stream.close device-42/boot-7",
                                                    "ledger/1: mddlog.ledger.close ledger/1"}),
                                    "origin, open, close, ledger.close");
                      const auto stream = rig->provider.inner.latest(streamP);
                      const auto ledger = rig->provider.inner.latest("ledger/1");
                      checks.expect(std::holds_alternative<Anchor>(stream) && std::get<Anchor>(stream).position == 5,
                                    "the stream is anchored at its last position");
                      checks.expect(std::holds_alternative<Anchor>(ledger) && std::get<Anchor>(ledger).position == 4, "so is the ledger, through ledger.close");
                      checks.expect(rig->sink->health().counters.anchorsAccepted == 2, "two anchors accepted");
                      checks.expect(!rig->sink->accept(makeEvent(streamP, 6)), "a closed stream accepts nothing more");
                      checks.raise();
                  })
            .Then("the next start cites the earlier ledger at its last position and the stream at its close, and the new stream begins at sequence 1",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      orderlySession(*rig);
                      checks.expect(rig->start(), "restarted");
                      const auto& restart = rig->sink->restart();
                      checks.expect(restart.firstRecord == LedgerRecordKind::Predecessor && restart.predecessor == "ledger/1" && restart.predecessorChecks
                                        && restart.predecessorPosition == 4 && restart.recoveredChecked == 1 && restart.recoveredFailed == 0
                                        && restart.startWritten,
                                    "predecessor at 4, one recovered stream");
                      checks.expect(rig->feed(streamQ, 1, 3) == 3, "the new instance takes sequence 1 again under a new identity");
                      const Parsed parsed{*rig};
                      const auto*  second = parsed.log.ledger("ledger/2");
                      const auto*  first  = parsed.log.ledger("ledger/1");
                      checks.expect(second != nullptr && first != nullptr && second->predecessor && second->predecessor->checks
                                        && second->predecessor->position == 4 && second->predecessor->digest == first->headDigest,
                                    "the citation is the earlier ledger's head digest at position 4");
                      checks.expect(second != nullptr && second->recovered.size() == 1 && second->recovered[0].target == streamP
                                        && second->recovered[0].position == 5 && second->recovered[0].digest == chainDigestAt(streamP, 5),
                                    "the recovered record is H_5 of the stream");
                      checks.expect(parsed.log.newestLedger() == std::optional<std::string>{"ledger/2"}, "ledger/2 is the newest");
                      checks.raise();
                  })
            .Then("a reader reports each instance on its own, and a matched citation is never a continuity guarantee",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      orderlySession(*rig);
                      checks.expect(rig->start(), "restarted");
                      (void)rig->feed(streamQ, 1, 3);
                      const auto  report    = readLog(*rig);
                      const auto* oldStream = report.find(streamP);
                      const auto* newStream = report.find(streamQ);
                      checks.expect(oldStream != nullptr && oldStream->report.verdict == Verdict::Anchored
                                        && oldStream->report.anchoredThrough == std::uint64_t{5},
                                    "the old instance is anchored through 5");
                      checks.expect(newStream != nullptr && newStream->report.verdict == Verdict::Unanchored && newStream->report.firstRetained == 1
                                        && newStream->report.lastPresent == 3,
                                    "the new instance is a separate report, from sequence 1, unanchored");
                      checks.expect(report.streams.size() == 4, "two streams and two ledgers, none folded into another");
                      const auto closed = report.notesOf(BoundaryKind::ClosedAt);
                      checks.expect(closed.size() == 1 && closed[0].stream == streamP && closed[0].position == 5 && closed[0].ledger == "ledger/1",
                                    "closed at 5");
                      const auto predecessor = report.notesOf(BoundaryKind::PredecessorMatched);
                      checks.expect(predecessor.size() == 1 && predecessor[0].stream == "ledger/2" && predecessor[0].other == "ledger/1"
                                        && predecessor[0].position == 4,
                                    "predecessor ledger/1 cited at 4, matched");
                      checks.expect(report.has(BoundaryKind::RecoveredMatched, "ledger/2"), "the recovered citation matches");
                      checks.expect(report.has(BoundaryKind::NoCloseRecorded, streamQ), "the newest ledger has not closed the new stream");
                      checks.expect(report.unlisted.empty(), "no stream is missing");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register abruptRestart{
    "A restart after an abrupt end reports what ended without a close, and what may have been lost cannot be told from what was removed",
    "integration",
    [] {
        return speclab::Test("audit-restart-abrupt")
            .Then("the earlier ledger has no close, the stream ended without one, and the citations still match what the medium holds",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      (void)rig->feed(streamP, 1, 5);
                      rig->powerLoss();
                      checks.expect(rig->start(), "restarted");
                      const auto& restart = rig->sink->restart();
                      checks.expect(restart.predecessor == "ledger/1" && restart.predecessorChecks && restart.predecessorPosition == 2
                                        && restart.recoveredChecked == 1,
                                    "predecessor ledger/1 at its 2 durable records, the stream recovered");
                      const auto report = readLog(*rig);
                      checks.expect(report.has(BoundaryKind::LedgerNotClosed, "ledger/1"), "no ledger.close in the earlier ledger");
                      const auto ended = report.notesOf(BoundaryKind::EndedWithoutClose);
                      checks.expect(ended.size() == 1 && ended[0].stream == streamP && ended[0].position == 5, "ended without close at 5");
                      checks.expect(report.has(BoundaryKind::PredecessorMatched, "ledger/2") && report.has(BoundaryKind::RecoveredMatched, "ledger/2"),
                                    "the restart citations are compared with what is stored, and match");
                      const auto* stream = report.find(streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Unanchored && stream->report.lastPresent == 5,
                                    "no anchor was ever advanced");
                      checks.raise();
                  })
            .Then(
                "records handed off but never confirmed are not in the log after a power loss, and nothing says they ever were",
                [] {
                    speclab::core::Checks checks;
                    auto                  rig = makeRig();
                    checks.expect(rig->start(100), "a policy that never syncs on its own");
                    (void)rig->feed(streamP, 1, 4);
                    rig->powerLoss(Unconfirmed::Dropped);
                    checks.expect(rig->start(), "restarted");
                    checks.expect(heldOf(rig->medium, streamP).count == 0, "nothing of the stream is stored");
                    checks.expect(rig->sink->restart().recoveredChecked == 0 && rig->sink->restart().recoveredFailed == 0, "so nothing is cited as recovered");
                    const auto report = readLog(*rig);
                    checks.expect(report.find(streamP) == nullptr && report.unlisted.empty(), "and no stream is reported: it was never anchored or confirmed");
                    checks.raise();
                })
            .Execute();
    }};

const speclab::Register absentState{"An absent chain state starts at origin, and a reader says whether earlier history is known to exist", "integration", [] {
                                        return speclab::Test("audit-restart-absent")
                                            .Then("a log wiped while the provider still holds anchors is origin claimed while earlier history exists",
                                                  [] {
                                                      speclab::core::Checks checks;
                                                      auto                  before = makeRig();
                                                      orderlySession(*before);
                                                      auto after = makeRig();
                                                      after->provider.inner.restore(before->provider.inner.snapshot());
                                                      after->session = 5;
                                                      checks.expect(after->start(), "started on an empty medium");
                                                      const auto& restart = after->sink->restart();
                                                      checks.expect(restart.firstRecord == LedgerRecordKind::Origin && restart.startWritten, "origin");
                                                      checks.expect(hasFault(restart.faults, IntegrityFaultKind::AnchoredEvidenceGone, streamP)
                                                                        && hasFault(restart.faults, IntegrityFaultKind::AnchoredEvidenceGone, "ledger/1"),
                                                                    "anchored evidence has gone: an integrity fault for each stream");
                                                      const auto health = after->sink->health();
                                                      checks.expect(health.counters.integrityFaults == 2 && health.lastIssue == StorageIssue::IntegrityFault
                                                                        && health.integrity.size() == 2,
                                                                    "observable through health, never as an audit event");
                                                      const auto report = readLog(*after);
                                                      checks.expect(report.has(BoundaryKind::OriginClaimedWhileHistoryExists, "ledger/6"),
                                                                    "origin claimed while earlier history exists");
                                                      const auto* lost = report.findUnlisted(streamP);
                                                      checks.expect(lost != nullptr && lost->verdict == Verdict::Incomplete && lost->lastPresent == 0
                                                                        && lost->cause == VerdictCause::LogEndsBeforeAnchor,
                                                                    "the anchored stream is Incomplete with m = 0");
                                                      checks.raise();
                                                  })
                                            .Then("records of a stream no ledger opened are an inconsistent state: reported, never repaired",
                                                  [] {
                                                      speclab::core::Checks   checks;
                                                      auto                    rig = makeRig();
                                                      std::vector<AuditEvent> events;
                                                      for (std::uint64_t k = 1; k <= 4; ++k)
                                                          events.push_back(makeEvent(streamP, k));
                                                      const auto segment = forgeSegment(rig->medium, streamP, events);
                                                      const auto before  = rig->medium.bytesOf(segment);
                                                      checks.expect(rig->start(), "started");
                                                      const auto& restart = rig->sink->restart();
                                                      checks.expect(restart.firstRecord == LedgerRecordKind::Origin, "no ledger held: origin");
                                                      checks.expect(hasFault(restart.faults, IntegrityFaultKind::StreamNotOpened, streamP),
                                                                    "an integrity fault for the stream no ledger opened");
                                                      checks.expect(rig->medium.bytesOf(segment) == before, "the evidence is untouched");
                                                      const auto report = readLog(*rig);
                                                      checks.expect(report.has(BoundaryKind::StreamNotOpenedByAnyLedger, streamP), "a reader reports it too");
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};

/** @brief A log written straight to a medium: ledger/1 that opened and closed P, and P itself, optionally with a rewritten record. */
struct ForgedLog {
    std::vector<AuditEvent> stream;
    std::vector<AuditEvent> ledger;
};

[[nodiscard]] ForgedLog forgedLog(std::uint64_t count = 5) {
    ForgedLog out;
    for (std::uint64_t k = 1; k <= count; ++k)
        out.stream.push_back(makeEvent(streamP, k));
    out.ledger.push_back(ledgerEvent(LedgerEntry::origin("ledger/1"), "ledger/1", 1));
    out.ledger.push_back(ledgerEvent(LedgerEntry::streamOpen(streamP), "ledger/1", 2));
    out.ledger.push_back(ledgerEvent(LedgerEntry::streamClose(streamP, count, chainDigestAt(streamP, count)), "ledger/1", 3));
    out.ledger.push_back(ledgerEvent(LedgerEntry::ledgerClose("ledger/1"), "ledger/1", 4));
    return out;
}

const speclab::Register inconsistentState{
    "An inconsistent chain state is written as the Failed form, cites the last position that checks, and the evidence is never touched",
    "integration",
    [] {
        return speclab::Test("audit-restart-inconsistent")
            .Then("a stored digest that the records do not reproduce: the Failed citation names the last position that checks",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig    = makeRig();
                      const auto            forged = forgedLog();
                      (void)forgeSegment(rig->medium, "ledger/1", forged.ledger);
                      const auto segment = forgeSegment(rig->medium, streamP, forged.stream, 0, 0, std::size_t{2});
                      const auto before  = rig->medium.bytesOf(segment);
                      rig->session       = 1;
                      checks.expect(rig->start(), "started");
                      const auto& restart = rig->sink->restart();
                      checks.expect(restart.predecessor == "ledger/1" && restart.predecessorChecks, "the ledger itself checks");
                      checks.expect(restart.recoveredChecked == 0 && restart.recoveredFailed == 1, "the stream is cited in its Failed form");
                      checks.expect(hasFault(restart.faults, IntegrityFaultKind::ChainDigestMismatch, streamP)
                                        && hasFault(restart.faults, IntegrityFaultKind::CloseCitationMismatch, streamP),
                                    "a digest mismatch, and a close that the records do not reproduce");
                      const Parsed parsed{*rig};
                      const auto*  mine = parsed.log.ledger("ledger/2");
                      checks.expect(mine != nullptr && mine->recovered.size() == 1 && !mine->recovered[0].checks && mine->recovered[0].position == 2
                                        && mine->recovered[0].digest == chainDigestAt(streamP, 2),
                                    "Failed, citing position 2 and H_2");
                      checks.expect(rig->medium.bytesOf(segment) == before, "the stream's segment is byte for byte what it was");
                      const auto  report = readLog(*rig);
                      const auto* stream = report.find(streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Inconsistent && stream->report.failedAt == std::uint64_t{3},
                                    "the reader finds it Inconsistent at record 3, recomputed on its own");
                      checks.expect(report.has(BoundaryKind::RecoveredFailedForm, "ledger/2"), "and states the Failed citation");
                      checks.raise();
                  })
            .Then("a failed continuity check cites no position at all, and the reader finds the stream Altered against the anchor",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      orderlySession(*rig);
                      Anchor wrong;
                      wrong.anchorFormat     = anchorFormatVersion;
                      wrong.canonicalVersion = canonicalContractVersion;
                      wrong.streamId         = std::string{streamP};
                      wrong.position         = 5;
                      wrong.digest           = chainDigestAt(streamP, 4);
                      wrong.providerId       = "witness-1";
                      wrong.counter          = 100;
                      rig->provider.inner.inject(wrong);
                      checks.expect(rig->start(), "restarted");
                      const auto& restart = rig->sink->restart();
                      checks.expect(restart.recoveredFailed == 1 && hasFault(restart.faults, IntegrityFaultKind::ContinuityFailed, streamP),
                                    "the continuity check failed");
                      const Parsed parsed{*rig};
                      const auto*  mine = parsed.log.ledger("ledger/2");
                      checks.expect(mine != nullptr && mine->recovered.size() == 1 && !mine->recovered[0].checks && !mine->recovered[0].position
                                        && !mine->recovered[0].digest,
                                    "no position of that stream is recovered");
                      const auto  report = readLog(*rig);
                      const auto* stream = report.find(streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Altered
                                        && stream->report.cause == VerdictCause::AnchorDigestMismatch,
                                    "Altered against the anchor");
                      checks.raise();
                  })
            .Then("a close record the stored records do not reproduce makes the stream inconsistent and the removal of its tail visible",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      orderlySession(*rig);
                      const auto segment = segmentsOf(rig->medium, streamP).front().ref;
                      const auto bytes   = rig->medium.bytesOf(segment);
                      rig->medium.truncate(segment, scanSegment(bytes).records.back().frameOffset);
                      checks.expect(rig->start(), "restarted");
                      checks.expect(hasFault(rig->sink->restart().faults, IntegrityFaultKind::CloseCitationMismatch, streamP),
                                    "the close cites a record that is gone");
                      checks.expect(rig->sink->restart().recoveredFailed == 1, "Failed form");
                      const auto  report = readLog(*rig);
                      const auto* stream = report.find(streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Incomplete
                                        && stream->report.cause == VerdictCause::LogEndsBeforeAnchor && stream->report.lastPresent == 4,
                                    "Incomplete: the anchor covers 5 and the log ends at 4");
                      checks.expect(report.has(BoundaryKind::ClosedCitationNotReproduced, streamP), "and the close is reported as not reproduced");
                      checks.raise();
                  })
            .Then("two ledgers that no other cites are a fork, reported and never resolved by choosing one",
                  [] {
                      speclab::core::Checks checks;
                      auto                  one = makeRig();
                      checks.expect(one->start(), "first log");
                      auto two     = makeRig();
                      two->session = 1;
                      checks.expect(two->start(), "second log, another ledger identity");
                      auto merged     = makeRig();
                      merged->session = 2;
                      copySegments(one->medium, merged->medium);
                      copySegments(two->medium, merged->medium);
                      const auto before = readLog(*merged);
                      const auto forks  = before.notesOf(BoundaryKind::LedgersFork);
                      checks.expect(forks.size() == 2, "a reader reports both ledgers");
                      checks.expect(merged->start(), "restarted");
                      const auto& restart = merged->sink->restart();
                      checks.expect(restart.fork && restart.firstRecord == LedgerRecordKind::Predecessor && !restart.predecessorChecks,
                                    "the predecessor is cited in its Failed form");
                      checks.expect(hasFault(restart.faults, IntegrityFaultKind::LedgersFork, "ledger/1")
                                        && hasFault(restart.faults, IntegrityFaultKind::LedgersFork, "ledger/2"),
                                    "both ledgers are reported through health");
                      checks.raise();
                  })
            .Then("a malformed ledger record makes the ledger Inconsistent at that record, and the next ledger cites it as not checking",
                  [] {
                      speclab::core::Checks    checks;
                      auto                     rig    = makeRig();
                      auto                     forged = forgedLog();
                      mddlog::core::AuditInput bad;
                      bad.action = ledgeraction::streamClose;
                      bad.actor  = "alice";
                      bad.target = streamP;
                      AuditEvent event;
                      (void)event.assign(bad, "ledger/1", 3);
                      forged.ledger[2] = event;
                      (void)forgeSegment(rig->medium, "ledger/1", forged.ledger);
                      (void)forgeSegment(rig->medium, streamP, forged.stream);
                      rig->session = 1;
                      checks.expect(rig->start(), "started");
                      checks.expect(rig->sink->restart().predecessor == "ledger/1" && !rig->sink->restart().predecessorChecks
                                        && hasFault(rig->sink->restart().faults, IntegrityFaultKind::LedgerRecordMalformed, "ledger/1"),
                                    "predecessor in its Failed form, and a fault");
                      const auto  report = readLog(*rig);
                      const auto* ledger = report.find("ledger/1");
                      checks.expect(ledger != nullptr && ledger->report.verdict == Verdict::Inconsistent
                                        && ledger->report.cause == VerdictCause::LedgerRecordMalformed && ledger->report.failedAt == std::uint64_t{3},
                                    "Inconsistent at record 3");
                      checks.expect(report.has(BoundaryKind::LedgerRecordMalformed, "ledger/1"), "named as a boundary finding");
                      const auto* stream = report.find(streamP);
                      checks.expect(stream != nullptr && stream->report.verdict == Verdict::Unanchored,
                                    "the malformed close ended no stream, and the stream still verifies on its own");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register readerGuard{"A reserved action outside a ledger is reported by a reader, and is not interpreted", "unit", [] {
                                        return speclab::Test("audit-restart-reserved-outside")
                                            .Then("a stream whose first record is not origin or predecessor is no ledger, whatever it holds",
                                                  [] {
                                                      speclab::core::Checks   checks;
                                                      auto                    rig = makeRig();
                                                      std::vector<AuditEvent> events;
                                                      events.push_back(makeEvent(streamP, 1));
                                                      events.push_back(ledgerEvent(LedgerEntry::streamOpen(streamQ), streamP, 2));
                                                      (void)forgeSegment(rig->medium, streamP, events);
                                                      const auto report = readLog(*rig);
                                                      const auto notes  = report.notesOf(BoundaryKind::ReservedActionOutsideLedger);
                                                      checks.expect(notes.size() == 1 && notes[0].stream == streamP && notes[0].position == 2,
                                                                    "reported at record 2");
                                                      const Parsed parsed{*rig};
                                                      checks.expect(!parsed.log.isLedger(streamP), "and it is not a ledger");
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};

const speclab::Register configuration{
    "The ledger's identity and failures are checked and observable",
    "unit",
    [] {
        return speclab::Test("audit-restart-configuration")
            .Then("an identity AuditEvent rejects, or one the medium already holds, is refused",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig      = makeRig();
                      auto                  declared = rig->config();
                      LedgerConfig          ledgerConfig;
                      ledgerConfig.streamId = "not a valid id";
                      declared.ledger       = ledgerConfig;
                      checks.expect(PersistingAuditSink::create(rig->medium, declared).error() == StorageConfigError::InvalidLedgerStream, "invalid identity");
                      checks.expect(rig->start(), "started");
                      auto again            = rig->config();
                      ledgerConfig.streamId = "ledger/1";
                      again.ledger          = ledgerConfig;
                      checks.expect(PersistingAuditSink::create(rig->medium, again).error() == StorageConfigError::LedgerIdentityInUse,
                                    "a restart starts a new ledger");
                      checks.raise();
                  })
            .Then("a producer stream cannot take the ledger's identity",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      checks.expect(!rig->sink->accept(makeEvent("ledger/1", 1)), "refused");
                      checks.expect(rig->sink->health().counters.streamIdentityInUse == 1, "counted as an identity in use");
                      checks.raise();
                  })
            .Then("a ledger that cannot write its start is failed, observable, and no producer event is stored",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->medium.inject({.operation = Operation::Append, .ordinal = 1, .effect = Effect::Fail});
                      checks.expect(rig->start(), "the sink is built");
                      checks.expect(!rig->sink->restart().startWritten, "but the start was not written");
                      const auto  health = rig->sink->health();
                      const auto* mine   = healthOf(health, "ledger/1");
                      checks.expect(mine != nullptr && mine->state == StreamStorageState::Failed && mine->cause == StorageIssue::AppendFailed,
                                    "the ledger failed on the append");
                      checks.expect(!rig->sink->accept(makeEvent(streamP, 1)) && rig->sink->health().counters.ledgerRefused == 1,
                                    "producers are refused, never blocked");
                      checks.expect(heldOf(rig->medium, streamP).count == 0 && rig->losses.empty(), "nothing stored, and no event was lost after admission");
                      checks.raise();
                  })
            .Then("closed streams stop counting against S, and close without a failed stream writes ledger.close",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      (void)rig->feed(streamP, 1, 2);
                      (void)rig->feed(streamQ, 1, 2);
                      checks.expect(!rig->sink->accept(makeEvent(streamR, 1)), "S = 2: a third open stream is refused");
                      checks.expect(rig->sink->closeStream(streamP), "an orderly close");
                      checks.expect(rig->sink->accept(makeEvent(streamR, 1)), "the closed stream no longer counts");
                      rig->sink->close();
                      const auto actionsNow = ledgerActions(rig->medium);
                      checks.expect(!actionsNow.empty() && actionsNow.back() == "ledger/1: mddlog.ledger.close ledger/1",
                                    "every stream closed: ledger.close is written");
                      checks.raise();
                  })
            .Then("a stream that failed leaves the session without ledger.close, which a reader reports as an abrupt end",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      (void)rig->feed(streamP, 1, 2);
                      (void)rig->feed(streamQ, 1, 1);
                      // The next producer append fails: Q ends on a storage failure.
                      rig->medium.inject({.operation = Operation::Append, .ordinal = rig->medium.calls(Operation::Append) + 1, .effect = Effect::Fail});
                      checks.expect(!rig->sink->accept(makeEvent(streamQ, 2)), "refused");
                      rig->sink->close();
                      checks.expect(!std::ranges::any_of(ledgerActions(rig->medium),
                                                         [](const std::string& line) {
                                                             return line.ends_with("mddlog.ledger.close ledger/1");
                                                         }),
                                    "no ledger.close");
                      checks.expect(readLog(*rig).has(BoundaryKind::LedgerNotClosed, "ledger/1"), "reported as not closed");
                      checks.raise();
                  })
            .Execute();
    }};

/** @brief One scripted restart over a log of two orderly-closed streams, cut at one write or acknowledgment point of the second start. */
struct CutCase {
    Operation   operation;
    std::size_t ordinal;
    Effect      effect;
    std::size_t partialBytes = 0;
};

const speclab::Register cutsDuringRestart{
    "Power lost at any write or acknowledgment point of a restart leaves a log the next restart recovers and a reader reads without a finding",
    "integration",
    [] {
        return speclab::Test("audit-restart-cuts")
            .Then("every cut point of the second start, then a third start",
                  [] {
                      speclab::core::Checks checks;
                      const auto            prepare = [] {
                          auto rig = makeRig();
                          (void)rig->start();
                          (void)rig->feed(streamP, 1, 3);
                          (void)rig->feed(streamQ, 1, 2);
                          rig->sink->close();
                          return rig;
                      };
                      // A dry run counts the calls of the second start.
                      auto       dry          = prepare();
                      const auto openBefore   = dry->medium.calls(Operation::Open);
                      const auto appendBefore = dry->medium.calls(Operation::Append);
                      const auto syncBefore   = dry->medium.calls(Operation::Sync);
                      (void)dry->start();
                      std::vector<CutCase> cases;
                      for (auto at = openBefore + 1; at <= dry->medium.calls(Operation::Open); ++at) {
                          cases.push_back({Operation::Open, at, Effect::CutBefore, 0});
                          cases.push_back({Operation::Open, at, Effect::CutPartial, 10});
                          cases.push_back({Operation::Open, at, Effect::CutAfter, 0});
                      }
                      for (auto at = appendBefore + 1; at <= dry->medium.calls(Operation::Append); ++at) {
                          cases.push_back({Operation::Append, at, Effect::CutBefore, 0});
                          cases.push_back({Operation::Append, at, Effect::CutPartial, 20});
                          cases.push_back({Operation::Append, at, Effect::CutAfter, 0});
                      }
                      for (auto at = syncBefore + 1; at <= dry->medium.calls(Operation::Sync); ++at) {
                          cases.push_back({Operation::Sync, at, Effect::CutBefore, 0});
                          cases.push_back({Operation::Sync, at, Effect::CutAfter, 0});
                      }
                      checks.expect(cases.size() >= 12, "the second start has several write and acknowledgment points");

                      bool restarted  = true;
                      bool consistent = true;
                      bool streamsOk  = true;
                      for (const CutCase& cut : cases) {
                          auto rig = prepare();
                          rig->medium.inject({.operation = cut.operation, .ordinal = cut.ordinal, .effect = cut.effect, .partialBytes = cut.partialBytes});
                          (void)rig->start();
                          rig->powerLoss();
                          restarted         = restarted && rig->start() && rig->sink->restart().startWritten;
                          const auto report = readLog(*rig);
                          for (const auto& item : report.streams)
                              consistent = consistent && item.report.verdict != Verdict::Inconsistent && item.report.verdict != Verdict::CannotVerify;
                          for (const auto id : {streamP, streamQ}) {
                              const auto* found = report.find(id);
                              streamsOk         = streamsOk && found != nullptr && found->report.verdict == Verdict::Anchored;
                          }
                          streamsOk = streamsOk && report.unlisted.empty();
                      }
                      checks.expect(restarted, "the third start always writes its record 1 and recovered records");
                      checks.expect(consistent, "no ledger or stream is Inconsistent or unverifiable after any cut");
                      checks.expect(streamsOk, "the earlier streams stay Anchored through their closed positions, and none is missing");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register citationCycles{
    "Ledgers whose citations form a cycle are an inconsistent state, never absent state",
    "integration",
    [] {
        return speclab::Test("audit-restart-cycle")
            .Then("a ledger that cites itself: a reader states the cycle, and the next start cites it in Failed form instead of writing origin",
                  [] {
                      speclab::core::Checks   checks;
                      auto                    rig = makeRig();
                      std::vector<AuditEvent> events{ledgerEvent(LedgerEntry::predecessor("ledger/0", false, std::nullopt, std::nullopt), "ledger/0", 1)};
                      (void)forgeSegment(rig->medium, "ledger/0", events);
                      const auto before = readLog(*rig);
                      checks.expect(before.has(BoundaryKind::LedgerCitationCycle, "ledger/0"), "the reader states the cycle");
                      checks.expect(rig->start(), "started");
                      const RestartReport& restart = rig->sink->restart();
                      checks.expect(restart.firstRecord == LedgerRecordKind::Predecessor && restart.predecessor == "ledger/0" && !restart.predecessorChecks,
                                    "record 1 cites the ledger in Failed form, never origin");
                      checks.expect(hasFault(restart.faults, IntegrityFaultKind::LedgerCitationCycle, "ledger/0"), "an integrity fault is reported");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register malformedPredecessor{"A predecessor with a malformed record is cited up to the record before it", "integration", [] {
                                                 return speclab::Test("audit-restart-malformed-predecessor")
                                                     .Then("the Failed form cites the last valid record, not the chain's head",
                                                           [] {
                                                               speclab::core::Checks checks;
                                                               auto                  rig       = makeRig();
                                                               LedgerEntry           malformed = LedgerEntry::streamOpen(streamQ);
                                                               malformed.sourceSequence        = 3;  // `open` carries no position (10.2)
                                                               std::vector<AuditEvent> events{ledgerEvent(LedgerEntry::origin("ledger/0"), "ledger/0", 1),
                                                                                              ledgerEvent(LedgerEntry::streamOpen(streamP), "ledger/0", 2),
                                                                                              ledgerEvent(malformed, "ledger/0", 3),
                                                                                              ledgerEvent(LedgerEntry::streamOpen(streamR), "ledger/0", 4)};
                                                               (void)forgeSegment(rig->medium, "ledger/0", events);
                                                               checks.expect(rig->start(), "started");
                                                               const RestartReport& restart = rig->sink->restart();
                                                               checks.expect(restart.predecessor == "ledger/0" && !restart.predecessorChecks
                                                                                 && restart.predecessorPosition == 2,
                                                                             "Failed, citing position 2");
                                                               checks.expect(hasFault(restart.faults, IntegrityFaultKind::LedgerRecordMalformed, "ledger/0"),
                                                                             "the malformed record is a fault");
                                                               checks.raise();
                                                           })
                                                     .Execute();
                                             }};

const speclab::Register identityReuse{"An identity a ledger names never starts a new instance, even after its records were removed", "integration", [] {
                                          return speclab::Test("audit-restart-identity-reuse")
                                              .Then("a removed stream's identity is refused at the next start, so its trims are never read as a new instance's",
                                                    [] {
                                                        speclab::core::Checks checks;
                                                        auto                  rig = makeRig();
                                                        checks.expect(rig->start(), "first start");
                                                        (void)rig->feed(streamP, 1, 3);
                                                        checks.expect(rig->sink->closeStream(streamP), "closed");
                                                        checks.expect(rig->sink->removeStream(streamP).outcome == RetentionOutcome::Removed,
                                                                      "removed as a whole");
                                                        rig->sink->close();
                                                        checks.expect(rig->start(), "second start");
                                                        checks.expect(!rig->sink->accept(makeEvent(streamP, 1)), "the identity is refused");
                                                        checks.expect(rig->sink->health().counters.streamIdentityInUse == 1,
                                                                      "and counted as an identity in use");
                                                        checks.expect(rig->sink->accept(makeEvent(streamQ, 1)), "a new identity is accepted");
                                                        checks.raise();
                                                    })
                                              .Execute();
                                      }};

}  // namespace
