/** @brief ADR-004 Decision 10.1 and 10.2: the reserved namespace, ledger records, and the rules that make a ledger record malformed (issue #92). */

import std;
import speclab;
import mddlog;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;
import mddlog.core.auditring;

#include "../framework/AuditLogRig.hpp"

namespace {

using namespace mddlog::spec::auditlog;
using mddlog::core::AuditCategory;
using mddlog::core::AuditField;
using mddlog::core::AuditInput;
using mddlog::core::AuditPhase;
using mddlog::core::AuditRefusalReason;
using mddlog::core::AuditRing;

constexpr std::string_view producer = "device-42/boot-7";
constexpr std::string_view ledgerId = "ledger/1";

[[nodiscard]] AuditInput ordinary(std::string_view action = "therapy.rate.set") {
    AuditInput input;
    input.action = action;
    input.target = "pump/channel-A";
    return input;
}

/** @brief One ledger-shaped record, with every field a malformed record might get wrong. */
struct Shape {
    std::string                  action;
    std::string                  target;
    AuditCategory                category = AuditCategory::Lifecycle;
    AuditPhase                   phase    = AuditPhase::Executed;
    std::string                  actor;
    std::string                  requirement;
    std::string                  risk;
    std::string                  correlation;
    std::optional<std::uint64_t> source;
};

[[nodiscard]] std::string hex(const Sha256Digest& digest) {
    const auto text = digestToHex(digest);
    return std::string{text.data(), text.size()};
}

[[nodiscard]] AuditEvent shaped(const Shape& shape, std::uint64_t sequence) {
    AuditInput input;
    input.category       = shape.category;
    input.phase          = shape.phase;
    input.action         = shape.action;
    input.actor          = shape.actor;
    input.target         = shape.target;
    input.requirementRef = shape.requirement;
    input.riskRef        = shape.risk;
    input.correlationId  = shape.correlation;
    input.sourceSequence = shape.source;
    AuditEvent event;
    (void)event.assign(input, ledgerId, sequence);
    return event;
}

[[nodiscard]] Shape originShape() {
    return {.action = std::string{ledgeraction::origin}, .target = std::string{ledgerId}};
}
[[nodiscard]] Shape openShape(std::string_view stream) {
    return {.action = std::string{ledgeraction::streamOpen}, .target = std::string{stream}};
}
[[nodiscard]] Shape closeShape(std::string_view stream, std::uint64_t position) {
    return {.action = std::string{ledgeraction::streamClose}, .target = std::string{stream}, .correlation = hex(chainDigestAt(stream, 1)), .source = position};
}
[[nodiscard]] Shape trimShape(std::string_view stream, std::uint64_t position) {
    return {.action = std::string{ledgeraction::streamTrim}, .target = std::string{stream}, .correlation = hex(chainDigestAt(stream, 1)), .source = position};
}

/** @brief Feeds records one by one and answers what the checker said about each. */
class Feed {
public:
    Feed() : checker(ledgerId) {}

    [[nodiscard]] std::optional<LedgerFault> next(const Shape& shape) {
        const auto event     = shaped(shape, ++sequence);
        const auto canonical = CanonicalRecord::encode(event).value_or(CanonicalRecord{});
        const auto decoded   = decodeCanonical(canonical.bytes());
        return checker.check(decoded.record).fault;
    }

private:
    LedgerChecker checker;
    std::uint64_t sequence = 0;
};

/** @brief The fault the second record draws after a valid origin; empty when it is valid. */
[[nodiscard]] std::optional<LedgerFault> afterOrigin(const Shape& shape) {
    Feed feed;
    (void)feed.next(originShape());
    return feed.next(shape);
}

const speclab::Register reservedNamespace{
    "Producers cannot write under the reserved ledger namespace, and the adapter still can",
    "unit",
    [] {
        return speclab::Test("audit-ledger-reserved")
            .Then("producer admission refuses every action under mddlog. without consuming a sequence",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<8>          ring{producer};
                      bool                  allRefused = true;
                      for (const std::string_view action :
                           {"mddlog.ledger.origin", "mddlog.ledger.predecessor", "mddlog.stream.open", "mddlog.anything", "mddlog."}) {
                          const auto result = ring.tryRecord(ordinary(action));
                          allRefused        = allRefused && !result.wasAdmitted() && result.refusal()->reason == AuditRefusalReason::ReservedAction
                                       && result.refusal()->field == AuditField::Action && result.sequence() == 0;
                      }
                      checks.expect(allRefused, "refused with an explicit reason that names the action field");
                      checks.expect(ring.admittedCount() == 0 && ring.refusalCount() == 0, "nothing was admitted and the saturation counter did not move");
                      const auto ordinaryResult = ring.tryRecord(ordinary());
                      checks.expect(ordinaryResult.wasAdmitted() && ordinaryResult.sequence() == 1,
                                    "the next ordinary event takes sequence 1: no sequence was consumed");
                      checks.raise();
                  })
            .Then("the prefix is case-sensitive and needs the dot; an invalid identifier is still an invalid identifier",
                  [] {
                      speclab::core::Checks checks;
                      AuditRing<8>          ring{producer};
                      for (const std::string_view action : {"Mddlog.x", "mddlog", "mddlog-x", "x.mddlog.y", "MDDLOG.x"})
                          checks.expect(ring.tryRecord(ordinary(action)).wasAdmitted(), "admitted");
                      const auto invalid = ring.tryRecord(ordinary("mddlog. bad"));
                      checks.expect(!invalid.wasAdmitted() && invalid.refusal()->reason == AuditRefusalReason::InvalidIdentifier,
                                    "the grammar is checked first");
                      checks.expect(ring.admittedCount() == 5, "five admitted");
                      checks.raise();
                  })
            .Then("the logger's audit entry point refuses them too, and an ordinary action still passes",
                  [] {
                      speclab::core::Checks checks;
                      mddlog::SimpleLogger  logger{"audit-reserved", false};
                      AuditRing<4>          ring{producer};
                      logger.setAuditRing(ring);
                      const auto refused = logger.logAudit(ordinary("mddlog.ledger.origin"));
                      checks.expect(!refused.wasAdmitted() && refused.refusal()->reason == AuditRefusalReason::ReservedAction,
                                    "refused at the producer boundary");
                      const auto admitted = logger.logAudit(ordinary());
                      checks.expect(admitted.wasAdmitted() && admitted.sequence() == 1, "an ordinary action is admitted with the first sequence");
                      logger.clearAuditRing();
                      checks.raise();
                  })
            .Then("AuditEvent itself does not refuse the prefix: the adapter builds ledger records through its own path",
                  [] {
                      speclab::core::Checks checks;
                      AuditEvent            event;
                      const auto            result = event.assign(ordinary("mddlog.ledger.origin"), ledgerId, 1);
                      checks.expect(result.wasAdmitted() && event.action() == "mddlog.ledger.origin", "assign() builds it");
                      const auto built = buildLedgerEvent(LedgerEntry::origin(ledgerId), ledgerId, 1, mddlog::core::RawTime::unavailable());
                      checks.expect(built && built->action() == ledgeraction::origin && built->streamId() == ledgerId && built->target() == ledgerId,
                                    "buildLedgerEvent() builds record 1");
                      checks.expect(!buildLedgerEvent(LedgerEntry::origin(ledgerId), ledgerId, 0, mddlog::core::RawTime::unavailable()),
                                    "sequence 0 is never a record");
                      checks.raise();
                  })
            .Then("a persisting sink does not store a producer's reserved action either",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      checks.expect(rig->start(), "started");
                      AuditEvent forged;
                      (void)forged.assign(ordinary("mddlog.ledger.origin"), producer, 1);
                      checks.expect(!rig->sink->accept(forged), "refused at the sink, where it could pass for a ledger");
                      checks.expect(rig->sink->health().counters.reservedActionRefused == 1, "and counted");
                      checks.expect(heldOf(rig->medium, producer).count == 0, "nothing was stored under that identity");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register entries{
    "Every ledger record is an audit record whose fields carry the table of 10.2",
    "unit",
    [] {
        return speclab::Test("audit-ledger-entries")
            .Then("each kind builds the action, target, position and digest the table gives, as Lifecycle records",
                  [] {
                      speclab::core::Checks checks;
                      const Sha256Digest    digest = chainDigestAt(producer, 3);
                      struct Row {
                          LedgerEntry                  entry;
                          std::string_view             action;
                          AuditPhase                   phase;
                          std::string_view             target;
                          std::optional<std::uint64_t> source;
                          bool                         digestPresent;
                      };
                      const std::vector<Row> rows{
                          {LedgerEntry::origin(ledgerId), "mddlog.ledger.origin", AuditPhase::Executed, ledgerId, std::nullopt, false},
                          {LedgerEntry::predecessor("ledger/0", true, 9, digest), "mddlog.ledger.predecessor", AuditPhase::Executed, "ledger/0", 9, true},
                          {LedgerEntry::predecessor("ledger/0", false, 4, digest), "mddlog.ledger.predecessor", AuditPhase::Failed, "ledger/0", 4, true},
                          {LedgerEntry::predecessor("ledger/0", false, std::nullopt, std::nullopt),
                           "mddlog.ledger.predecessor", AuditPhase::Failed,
                           "ledger/0", std::nullopt,
                           false},
                          {LedgerEntry::recovered(producer, true, 3, digest), "mddlog.stream.recovered", AuditPhase::Executed, producer, 3, true},
                          {LedgerEntry::streamOpen(producer), "mddlog.stream.open", AuditPhase::Executed, producer, std::nullopt, false},
                          {LedgerEntry::streamClose(producer, 3, digest), "mddlog.stream.close", AuditPhase::Executed, producer, 3, true},
                          {LedgerEntry::streamTrim(producer, 3, digest), "mddlog.stream.trim", AuditPhase::Executed, producer, 3, true},
                          {LedgerEntry::ledgerClose(ledgerId), "mddlog.ledger.close", AuditPhase::Executed, ledgerId, std::nullopt, false}
                      };
                      bool          every = true;
                      std::uint64_t at    = 0;
                      for (const Row& row : rows) {
                          const auto event = buildLedgerEvent(row.entry, ledgerId, ++at, mddlog::core::RawTime::unavailable());
                          every            = every && event && event->action() == row.action && event->phase() == row.phase && event->target() == row.target
                                  && event->sourceSequence() == row.source && event->category() == AuditCategory::Lifecycle && event->actor().empty()
                                  && event->requirementRef().empty() && event->riskRef().empty()
                                  && (row.digestPresent ? event->correlationId() == hex(digest) : event->correlationId().empty());
                      }
                      checks.expect(every, "action, phase, target, source, digest in 64 lowercase hexadecimal characters, everything else empty");
                      checks.raise();
                  })
            .Then("a digest is its 64 lowercase hexadecimal characters, and nothing else reads back as one",
                  [] {
                      speclab::core::Checks checks;
                      const Sha256Digest    digest = chainDigestAt(producer, 2);
                      checks.expect(digestFromHex(hex(digest)) == digest, "round trip");
                      std::string upper = hex(digest);
                      std::ranges::transform(upper, upper.begin(), [](char ch) {
                          return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                      });
                      checks.expect(upper == hex(digest) || !digestFromHex(upper), "uppercase is refused");
                      checks.expect(!digestFromHex(hex(digest).substr(1)) && !digestFromHex(hex(digest) + "0") && !digestFromHex(""), "wrong lengths");
                      std::string bad = hex(digest);
                      bad[5]          = 'g';
                      checks.expect(!digestFromHex(bad), "a non-hexadecimal character");
                      checks.raise();
                  })
            .Then("a reader names the fields by their ledger meaning through the typed view",
                  [] {
                      speclab::core::Checks   checks;
                      LedgerChecker           checker{ledgerId};
                      const Sha256Digest      digest = chainDigestAt(producer, 3);
                      std::vector<AuditEvent> events;
                      events.push_back(shaped(originShape(), 1));
                      events.push_back(shaped(openShape(producer), 2));
                      Shape trim       = trimShape(producer, 3);
                      trim.correlation = hex(digest);
                      events.push_back(shaped(trim, 3));
                      std::vector<CanonicalRecord> canonicals;
                      canonicals.reserve(events.size());
                      for (const auto& event : events)
                          canonicals.push_back(CanonicalRecord::encode(event).value_or(CanonicalRecord{}));
                      std::vector<LedgerRecordKind>   kinds;
                      std::optional<LedgerRecordView> last;
                      bool                            faultFree = true;
                      for (const auto& canonical : canonicals) {
                          const auto decoded = decodeCanonical(canonical.bytes());
                          const auto checked = checker.check(decoded.record);
                          faultFree          = faultFree && !checked.fault.has_value() && checked.view.has_value();
                          if (checked.view) {
                              kinds.push_back(checked.view->kind);
                              if (checked.view->kind == LedgerRecordKind::StreamTrim) {
                                  checks.expect(checked.view->target == producer && checked.view->sourceSequence == 3 && checked.view->digest == digest,
                                                "the trim's target, position and digest");
                              }
                          }
                      }
                      checks.expect(faultFree, "a valid origin, open and trim");
                      checks.expect(kinds
                                        == std::vector<LedgerRecordKind>{LedgerRecordKind::Origin, LedgerRecordKind::StreamOpen, LedgerRecordKind::StreamTrim},
                                    "kinds in order");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register malformed{
    "Each way a ledger record can be malformed is found, and an unreadable record applies nothing",
    "unit",
    [] {
        return speclab::Test("audit-ledger-malformed")
            .Then("a wrong category or phase",
                  [] {
                      speclab::core::Checks checks;
                      Shape                 category = openShape(producer);
                      category.category              = AuditCategory::Access;
                      checks.expect(afterOrigin(category) == LedgerFault::WrongCategory, "not Lifecycle");
                      for (const auto phase : {AuditPhase::Requested, AuditPhase::Confirmed, AuditPhase::Failed}) {
                          Shape wrong = openShape(producer);
                          wrong.phase = phase;
                          checks.expect(afterOrigin(wrong) == LedgerFault::WrongPhase, "open is Executed only");
                      }
                      Shape failedClose = closeShape(producer, 1);
                      failedClose.phase = AuditPhase::Failed;
                      checks.expect(afterOrigin(failedClose) == LedgerFault::WrongPhase, "Failed is allowed for predecessor and recovered only");
                      Feed feed;
                      (void)feed.next({.action = std::string{ledgeraction::predecessor}, .target = "ledger/0", .phase = AuditPhase::Failed});
                      Shape failedRecovered = {.action = std::string{ledgeraction::recovered}, .target = std::string{producer}, .phase = AuditPhase::Failed};
                      checks.expect(!feed.next(failedRecovered).has_value(), "recovered may be Failed, citing nothing");
                      checks.raise();
                  })
            .Then("an actor, a requirement or a risk reference is never part of a ledger record",
                  [] {
                      speclab::core::Checks checks;
                      Shape                 actor = openShape(producer);
                      actor.actor                 = "alice";
                      Shape requirement           = openShape(producer);
                      requirement.requirement     = "REQ-1";
                      Shape risk                  = openShape(producer);
                      risk.risk                   = "HAZ-1";
                      checks.expect(afterOrigin(actor) == LedgerFault::FieldNotEmpty, "actor");
                      checks.expect(afterOrigin(requirement) == LedgerFault::FieldNotEmpty, "requirementRef");
                      checks.expect(afterOrigin(risk) == LedgerFault::FieldNotEmpty, "riskRef");
                      checks.raise();
                  })
            .Then(
                "a source position where none belongs, or none where one is required",
                [] {
                    speclab::core::Checks checks;
                    Shape                 openWithPosition = openShape(producer);
                    openWithPosition.source                = 3;
                    checks.expect(afterOrigin(openWithPosition) == LedgerFault::SourceSequenceRule, "open carries none");
                    Shape closeWithout = closeShape(producer, 3);
                    closeWithout.source.reset();
                    checks.expect(afterOrigin(closeWithout) == LedgerFault::SourceSequenceRule, "close requires one");
                    Shape trimWithout = trimShape(producer, 3);
                    trimWithout.source.reset();
                    checks.expect(afterOrigin(trimWithout) == LedgerFault::SourceSequenceRule, "trim requires one");
                    const Shape executedRecovered = {.action = std::string{ledgeraction::recovered}, .target = std::string{producer}};
                    Feed        feed;
                    (void)feed.next(
                        {.action = std::string{ledgeraction::predecessor}, .target = "ledger/0", .correlation = hex(chainDigestAt(producer, 1)), .source = 1});
                    checks.expect(feed.next(executedRecovered) == LedgerFault::SourceSequenceRule, "the Executed form of recovered cites a position");
                    Shape closeOfOrigin  = originShape();
                    closeOfOrigin.source = 1;
                    Feed first;
                    checks.expect(first.next(closeOfOrigin) == LedgerFault::SourceSequenceRule, "origin carries none");
                    checks.raise();
                })
            .Then("a correlation id that is not empty where it must be, or not 64 lowercase hexadecimal characters where it carries a digest",
                  [] {
                      speclab::core::Checks checks;
                      Shape                 openWithDigest = openShape(producer);
                      openWithDigest.correlation           = hex(chainDigestAt(producer, 1));
                      checks.expect(afterOrigin(openWithDigest) == LedgerFault::CorrelationRule, "open carries none");
                      Shape upper = closeShape(producer, 1);
                      std::ranges::transform(upper.correlation, upper.correlation.begin(), [](char ch) {
                          return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                      });
                      checks.expect(afterOrigin(upper) == LedgerFault::CorrelationRule, "uppercase");
                      Shape shortDigest = trimShape(producer, 1);
                      shortDigest.correlation.pop_back();
                      checks.expect(afterOrigin(shortDigest) == LedgerFault::CorrelationRule, "63 characters");
                      Shape emptyDigest = closeShape(producer, 1);
                      emptyDigest.correlation.clear();
                      checks.expect(afterOrigin(emptyDigest) == LedgerFault::CorrelationRule, "empty where a digest is required");
                      Feed feed;
                      checks.expect(
                          feed.next({.action = std::string{ledgeraction::predecessor}, .target = "ledger/0", .phase = AuditPhase::Failed, .source = 4})
                              == LedgerFault::CorrelationRule,
                          "a Failed citation names both its position and its digest, or neither");
                      checks.raise();
                  })
            .Then("an action under mddlog. that the table does not list, and an ordinary action in a ledger",
                  [] {
                      speclab::core::Checks checks;
                      checks.expect(afterOrigin({.action = "mddlog.stream.frob", .target = std::string{producer}}) == LedgerFault::UnknownReservedAction,
                                    "unknown");
                      checks.expect(afterOrigin({.action = "mddlog.ledger.origins", .target = std::string{ledgerId}}) == LedgerFault::UnknownReservedAction,
                                    "near miss");
                      checks.expect(afterOrigin({.action = "therapy.rate.set", .target = std::string{producer}}) == LedgerFault::NotALedgerAction, "ordinary");
                      checks.raise();
                  })
            .Then("origin or predecessor anywhere but record 1, record 1 that is neither, and a target that is not the ledger's own",
                  [] {
                      speclab::core::Checks checks;
                      checks.expect(afterOrigin(originShape()) == LedgerFault::HeadMisplaced, "origin at record 2");
                      checks.expect(afterOrigin({.action      = std::string{ledgeraction::predecessor},
                                                 .target      = "ledger/0",
                                                 .correlation = hex(chainDigestAt(producer, 1)),
                                                 .source      = 1})
                                        == LedgerFault::HeadMisplaced,
                                    "predecessor at record 2");
                      Feed feed;
                      checks.expect(feed.next(openShape(producer)) == LedgerFault::HeadMisplaced, "record 1 is not a head");
                      Shape foreign  = originShape();
                      foreign.target = "ledger/2";
                      Feed other;
                      checks.expect(other.next(foreign) == LedgerFault::TargetNotOwn, "origin names another ledger");
                      Shape foreignClose = {.action = std::string{ledgeraction::ledgerClose}, .target = "ledger/2"};
                      checks.expect(afterOrigin(foreignClose) == LedgerFault::TargetNotOwn, "ledger.close names another ledger");
                      checks.raise();
                  })
            .Then("recovered only straight after record 1 or another recovered, and nothing after ledger.close",
                  [] {
                      speclab::core::Checks checks;
                      Feed                  feed;
                      (void)feed.next(originShape());
                      (void)feed.next(openShape(producer));
                      checks.expect(feed.next({.action = std::string{ledgeraction::recovered}, .target = "device-42/boot-6", .phase = AuditPhase::Failed})
                                        == LedgerFault::RecoveredMisplaced,
                                    "recovered after an open");
                      Feed run;
                      (void)run.next({.action = std::string{ledgeraction::predecessor}, .target = "ledger/0", .phase = AuditPhase::Failed});
                      const auto recovered = Shape{.action = std::string{ledgeraction::recovered}, .target = "device-42/boot-6", .phase = AuditPhase::Failed};
                      checks.expect(!run.next(recovered).has_value() && !run.next(recovered).has_value(), "a run of recovered is fine");
                      Feed closed;
                      (void)closed.next(originShape());
                      (void)closed.next({.action = std::string{ledgeraction::ledgerClose}, .target = std::string{ledgerId}});
                      checks.expect(closed.next(openShape(producer)) == LedgerFault::RecordAfterLedgerClose, "a record after ledger.close");
                      checks.raise();
                  })
            .Then("per stream: close twice, anything but a trim after a close, trims that do not increase, and a trim past the close",
                  [] {
                      speclab::core::Checks checks;
                      Feed                  twice;
                      (void)twice.next(originShape());
                      (void)twice.next(openShape(producer));
                      (void)twice.next(closeShape(producer, 5));
                      checks.expect(twice.next(closeShape(producer, 5)) == LedgerFault::CloseTwice, "close twice");
                      checks.expect(twice.next(openShape(producer)) == LedgerFault::RecordAfterClose, "an open after the close");
                      checks.expect(!twice.next(trimShape(producer, 5)).has_value(), "a trim may follow the close, up to its position");
                      Feed beyond;
                      (void)beyond.next(originShape());
                      (void)beyond.next(openShape(producer));
                      (void)beyond.next(closeShape(producer, 5));
                      checks.expect(beyond.next(trimShape(producer, 6)) == LedgerFault::TrimBeyondClose, "a trim past the close position");
                      Feed increasing;
                      (void)increasing.next(originShape());
                      (void)increasing.next(openShape(producer));
                      (void)increasing.next(trimShape(producer, 4));
                      checks.expect(increasing.next(trimShape(producer, 4)) == LedgerFault::TrimNotIncreasing, "the same position again");
                      checks.expect(increasing.next(trimShape(producer, 3)) == LedgerFault::TrimNotIncreasing, "a lower position");
                      checks.expect(!increasing.next(trimShape(producer, 5)).has_value(), "a higher one");
                      checks.raise();
                  })
            .Then("a malformed record applies nothing: the close it describes does not end the stream",
                  [] {
                      speclab::core::Checks checks;
                      Feed                  feed;
                      (void)feed.next(originShape());
                      (void)feed.next(openShape(producer));
                      Shape badClose = closeShape(producer, 5);
                      badClose.actor = "alice";
                      checks.expect(feed.next(badClose) == LedgerFault::FieldNotEmpty, "malformed");
                      checks.expect(!feed.next(closeShape(producer, 5)).has_value(), "so a valid close is the first close");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
