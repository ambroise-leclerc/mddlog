/** @brief ADR-004 Decision 7: anchor provider, retained position and the verifier's coverage states (issue #90). */

import std;
import mddlog.adapter.auditverifier;
import mddlog.core.auditevent;
import speclab;

namespace {

// NOLINTBEGIN(hicpp-signed-bitwise): flipping a byte of stored test records.

using namespace mddlog::adapter;
using mddlog::core::AuditEvent;
using mddlog::core::RawTime;

constexpr std::string_view streamName = "device-42/boot-7";

/** @brief A written stream: canonical bytes and stored digests, as a reader would find them. */
struct Log {
    std::vector<std::vector<std::uint8_t>> bytes;
    std::vector<Sha256Digest>              digests;

    [[nodiscard]] std::vector<StoredRecord> records(std::size_t first = 0, std::size_t count = std::numeric_limits<std::size_t>::max()) const {
        std::vector<StoredRecord> out;
        for (std::size_t k = first; k < bytes.size() && out.size() < count; ++k)
            out.push_back({.bytes = bytes[k], .digest = digests[k]});
        return out;
    }
    /** @brief H_position, 1-based; H_0 for 0. */
    [[nodiscard]] Sha256Digest digestAt(std::size_t position) const {
        return position == 0 ? chainInitialValue : digests[position - 1];
    }
};

[[nodiscard]] Log writeLog(std::size_t count, std::string_view detailSuffix = "") {
    Log        log;
    AuditChain chain{streamName};
    for (std::size_t k = 1; k <= count; ++k) {
        AuditEvent event;
        const auto detail = "step " + std::to_string(k) + std::string{detailSuffix};
        (void)event.assign({.action = "therapy.rate.set", .target = "pump/channel-A", .detail = detail}, streamName, k);
        const auto chained = chain.append(event);
        log.bytes.emplace_back(chained->canonical.bytes().begin(), chained->canonical.bytes().end());
        log.digests.push_back(chained->digest);
    }
    return log;
}

/** @brief The same log with record `k` (1-based) altered and every later digest recomputed: the rewrite the anchor exists to catch. */
[[nodiscard]] Log rewriteWithRecompute(const Log& original, std::size_t k) {
    Log rewritten                  = original;
    rewritten.bytes[k - 1].back() ^= 0x01;  // flip detailTruncated
    Sha256Digest previous          = rewritten.digestAt(k - 1);
    for (std::size_t i = k - 1; i < rewritten.bytes.size(); ++i) {
        previous             = chainDigest(rewritten.bytes[i], previous);
        rewritten.digests[i] = previous;
    }
    return rewritten;
}

[[nodiscard]] AdvanceAnswer anchorAt(InMemoryAnchorProvider& provider, const Log& log, std::size_t position) {
    return provider.advance(makeAnchorClaim(streamName, position, log.digestAt(position)));
}

[[nodiscard]] StreamReport run(AnchorVerifier& verifier, const Log& log, std::size_t first = 0, std::size_t count = std::numeric_limits<std::size_t>::max()) {
    return verifier.verify(streamName, log.records(first, count));
}

[[nodiscard]] RawTime at(std::int64_t seconds) {
    return RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::seconds{seconds}});
}

const speclab::Register providerContract{
    "An anchor provider accepts monotonically and answers faithfully",
    "unit",
    [] {
        return speclab::Test("audit-anchor-provider")
            .Then("it stamps accepted claims with a strictly increasing counter",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(4);
                      InMemoryAnchorProvider provider{"witness-1"};
                      provider.setClock(at(100));
                      const auto  first  = anchorAt(provider, log, 2);
                      const auto  second = anchorAt(provider, log, 4);
                      const auto* a      = std::get_if<AnchorStamp>(&first);
                      const auto* b      = std::get_if<AnchorStamp>(&second);
                      checks.expect(a != nullptr && b != nullptr && a->counter == 1 && b->counter == 2 && a->providerId == "witness-1", "counters 1 then 2");
                      checks.expect(a != nullptr && a->acceptedTime.availability() == mddlog::core::TimeAvailability::Available,
                                    "the provider's clock is stamped");
                      const auto  latest = provider.latest(streamName);
                      const auto* held   = std::get_if<Anchor>(&latest);
                      checks.expect(held != nullptr && held->position == 4 && held->digest == log.digestAt(4) && held->usable(),
                                    "latest is the highest accepted anchor");
                      checks.raise();
                  })
            .Then("it refuses a position that does not rise, a conflicting digest and a malformed claim",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(4);
                      InMemoryAnchorProvider provider{"witness-1"};
                      (void)anchorAt(provider, log, 3);
                      const auto lower    = anchorAt(provider, log, 2);
                      const auto same     = anchorAt(provider, log, 3);
                      const auto conflict = provider.advance(makeAnchorClaim(streamName, 3, log.digestAt(4)));
                      auto       bad      = makeAnchorClaim(streamName, 0, log.digestAt(1));
                      checks.expect(std::get_if<AdvanceRefusal>(&lower) && *std::get_if<AdvanceRefusal>(&lower) == AdvanceRefusal::PositionNotIncreasing,
                                    "lower position");
                      checks.expect(std::get_if<AdvanceRefusal>(&same) && *std::get_if<AdvanceRefusal>(&same) == AdvanceRefusal::PositionNotIncreasing,
                                    "same claim again");
                      checks.expect(std::get_if<AdvanceRefusal>(&conflict) && *std::get_if<AdvanceRefusal>(&conflict) == AdvanceRefusal::Conflict,
                                    "same position, other digest");
                      const auto zero = provider.advance(bad);
                      bad.position    = 4;
                      bad.streamId    = "has space";
                      const auto name = provider.advance(bad);
                      checks.expect(std::get_if<AdvanceRefusal>(&zero) && *std::get_if<AdvanceRefusal>(&zero) == AdvanceRefusal::Malformed,
                                    "position 0 is malformed");
                      checks.expect(std::get_if<AdvanceRefusal>(&name) && *std::get_if<AdvanceRefusal>(&name) == AdvanceRefusal::Malformed,
                                    "an invalid stream identity is malformed");
                      const auto listing = provider.streams();
                      checks.expect(std::get<ProviderListing>(listing).head == 1, "refusals take no counter");
                      checks.raise();
                  })
            .Then("it retires a stream, keeps the retirement and accepts no more claims for it",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(4);
                      InMemoryAnchorProvider provider{"witness-1"};
                      (void)anchorAt(provider, log, 3);
                      const auto wrong   = provider.retire(streamName, 2);
                      const auto unknown = provider.retire("other/stream", 3);
                      checks.expect(std::get_if<RetireRefusal>(&wrong) && *std::get_if<RetireRefusal>(&wrong) == RetireRefusal::Conflict,
                                    "not the highest position");
                      checks.expect(std::get_if<RetireRefusal>(&unknown) && *std::get_if<RetireRefusal>(&unknown) == RetireRefusal::UnknownStream,
                                    "unknown stream");
                      const auto done = provider.retire(streamName, 3);
                      checks.expect(std::get_if<AnchorStamp>(&done) && std::get_if<AnchorStamp>(&done)->counter == 2, "the retirement takes the next counter");
                      const auto  latest = provider.latest(streamName);
                      const auto* gone   = std::get_if<Retirement>(&latest);
                      checks.expect(gone != nullptr && gone->finalAnchor.position == 3 && gone->finalAnchor.counter == 1 && gone->counter == 2,
                                    "final anchor and own counter");
                      const auto more = anchorAt(provider, log, 4);
                      checks.expect(std::get_if<AdvanceRefusal>(&more) != nullptr, "a retired stream accepts no further advance");
                      const auto listing = std::get<ProviderListing>(provider.streams());
                      checks.expect(listing.head == 2 && listing.entries.size() == 1 && std::holds_alternative<Retirement>(listing.entries.front()),
                                    "head never decreases, the retirement is listed");
                      checks.raise();
                  })
            .Then("an unavailable provider accepts nothing and says so",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(2);
                      InMemoryAnchorProvider provider{"witness-1"};
                      provider.setAvailable(false);
                      checks.expect(std::holds_alternative<ProviderUnavailable>(anchorAt(provider, log, 2)), "advance");
                      checks.expect(std::holds_alternative<ProviderUnavailable>(provider.retire(streamName, 2)), "retire");
                      checks.expect(std::holds_alternative<ProviderUnavailable>(provider.latest(streamName)), "latest");
                      checks.expect(std::holds_alternative<ProviderUnavailable>(provider.streams()), "streams");
                      provider.setAvailable(true);
                      checks.expect(std::holds_alternative<AnchorAbsent>(provider.latest(streamName)), "nothing was accepted meanwhile");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register coverageStates{
    "The verifier reports an explicit coverage state and never an unqualified pass",
    "unit",
    [] {
        return speclab::Test("audit-anchor-verifier")
            .Then("an anchor at the last record is Anchored through that position",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(5);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 5);
                      const auto report = run(verifier, log);
                      checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == std::uint64_t{5} && !report.staleByPosition(),
                                    "Anchored through 5");
                      checks.expect(report.firstRetained == 1 && report.lastPresent == 5, "coverage 1 … 5");
                      checks.expect(report.anchor.has_value() && report.anchor->providerId == "witness-1" && report.anchor->counter == 1
                                        && report.anchor->canonicalVersion == 1,
                                    "the report names the anchor's provider, counter and version");
                      checks.expect(report.rollbackNotExcluded(), "with no retained position, rollback is not excluded");
                      checks.raise();
                  })
            .Then("records past a stale anchor are internally consistent and unanchored",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 4);
                      const auto report = run(verifier, log);
                      checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == std::uint64_t{4},
                                    "coverage is limited to the anchor's position");
                      checks.expect(report.staleByPosition() && report.unanchoredFrom == std::uint64_t{5} && report.lastPresent == 6, "5 … 6 are unanchored");
                      checks.raise();
                  })
            .Then("an absent anchor is internally consistent but unanchored, never anchored",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(3);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      const auto             report = run(verifier, log);
                      checks.expect(report.verdict == Verdict::Unanchored && report.cause == VerdictCause::NoAnchor && !report.anchoredThrough.has_value(),
                                    "unanchored");
                      checks.expect(report.lastPresent == 3 && !report.rollbackNotExcluded(), "internally consistent through 3, and no claim of anchoring");
                      checks.expect(verdictName(report.verdict) == "internally consistent, unanchored", "its name is not verified");
                      checks.expect(retained.empty(), "an unanchored result retains nothing");
                      checks.raise();
                  })
            .Then("an unavailable provider is reported apart from an absent anchor",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(3);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      provider.setAvailable(false);
                      const auto report = run(verifier, log);
                      checks.expect(report.verdict == Verdict::AnchorUnavailable && report.cause == VerdictCause::ProviderUnavailable
                                        && report.lastPresent == 3,
                                    "anchor unavailable, with the same coverage as unanchored");
                      checks.raise();
                  })
            .Then("a rewrite with recomputed digests fails against the authentic anchor",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 5);
                      const Log  forged = rewriteWithRecompute(log, 2);
                      const auto report = run(verifier, forged);
                      checks.expect(report.verdict == Verdict::Altered && report.cause == VerdictCause::AnchorDigestMismatch, "Altered against the anchor");
                      checks.expect(!report.anchoredThrough.has_value() && report.chainFinding == ChainFinding::Ok,
                                    "every internal link checks out, which is why the anchor is required");
                      checks.expect(retained.empty(), "a failed verification retains nothing");
                      const Log  pastAnchor = rewriteWithRecompute(log, 6);
                      const auto late       = run(verifier, pastAnchor);
                      checks.expect(late.verdict == Verdict::Anchored && late.anchoredThrough == std::uint64_t{5},
                                    "an alteration past the anchor is not detectable: coverage stops at the anchor");
                      checks.raise();
                  })
            .Then("a broken internal link is Inconsistent before the provider is consulted",
                  [] {
                      speclab::core::Checks  checks;
                      Log                    log = writeLog(4);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 4);
                      log.bytes[2].back() ^= 0x01;
                      const auto report    = run(verifier, log);
                      checks.expect(report.verdict == Verdict::Inconsistent && report.chainFinding == ChainFinding::DigestMismatch
                                        && report.failedAt == std::uint64_t{3},
                                    "Inconsistent, naming record 3");
                      checks.expect(report.lastPresent == 2, "records 1 and 2 still count as internally consistent");
                      checks.raise();
                  })
            .Then("a deleted suffix covered by the anchor is Incomplete",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 5);
                      const auto report = run(verifier, log, 0, 3);
                      checks.expect(report.verdict == Verdict::Incomplete && report.cause == VerdictCause::LogEndsBeforeAnchor && report.lastPresent == 3,
                                    "records 4 … 5 are missing");
                      checks.expect(report.anchor.has_value() && report.anchor->position == 5 && !report.anchoredThrough.has_value(),
                                    "the prefix cannot be matched against the digest");
                      const auto none = verifier.verify(streamName, {});
                      checks.expect(none.verdict == Verdict::Incomplete && none.lastPresent == 0, "an anchored stream with no record is Incomplete with m = 0");
                      checks.raise();
                  })
            .Then("a suffix removed past the anchor cannot be told apart from records never written",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 3);
                      const auto report = run(verifier, log, 0, 5);
                      checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == std::uint64_t{3}
                                        && report.unanchoredFrom == std::uint64_t{4},
                                    "coverage is stated as 1 … 3, not as the end of the log");
                      checks.raise();
                  })
            .Then("an unusable anchor is Cannot verify, never absence and never a match",
                  [] {
                      speclab::core::Checks checks;
                      const Log             log = writeLog(3);
                      for (int variant = 0; variant < 3; ++variant) {
                          InMemoryAnchorProvider provider{"witness-1"};
                          RetainedPosition       retained;
                          AnchorVerifier         verifier{provider, retained};
                          Anchor                 anchor{.anchorFormat     = anchorFormatVersion,
                                                        .canonicalVersion = canonicalContractVersion,
                                                        .streamId         = std::string{streamName},
                                                        .position         = 3,
                                                        .digest           = log.digestAt(3),
                                                        .providerId       = "witness-1",
                                                        .counter          = 1};
                          if (variant == 0)
                              anchor.anchorFormat = 9;
                          else if (variant == 1)
                              anchor.canonicalVersion = 2;
                          else
                              anchor.providerId.clear();
                          provider.inject(anchor);
                          const auto report = run(verifier, log);
                          checks.expect(report.verdict == Verdict::CannotVerify, "variant " + std::to_string(variant) + " is Cannot verify");
                          checks.expect(variant == 2 ? report.cause == VerdictCause::AnchorMissingField : report.cause == VerdictCause::AnchorFormatUnknown,
                                        "the cause is named");
                          checks.expect(report.lastPresent == 3 && retained.empty(), "internal consistency only, and nothing retained");
                      }
                      checks.raise();
                  })
            .Then("an unknown stream version is Cannot verify with no coverage",
                  [] {
                      speclab::core::Checks checks;
                      Log                   log = writeLog(2);
                      log.bytes[0][1]           = 2;
                      log.bytes[1][1]           = 2;
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      const auto             report = run(verifier, log);
                      checks.expect(report.verdict == Verdict::CannotVerify && report.cause == VerdictCause::UnsupportedStreamVersion,
                                    "version 2 cannot be verified");
                      checks.raise();
                  })
            .Then("an invalid stream identity is a caller fault, not a finding",
                  [] {
                      speclab::core::Checks  checks;
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      const auto             report = verifier.verify("has space", {});
                      checks.expect(report.verdict == Verdict::CannotVerify && report.cause == VerdictCause::InvalidStreamIdentity,
                                    "cannot verify, with that cause");
                      checks.raise();
                  })
            .Then("age staleness warns without reducing coverage",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(3);
                      InMemoryAnchorProvider provider{"witness-1"};
                      provider.setClock(at(1000));
                      (void)anchorAt(provider, log, 3);
                      {
                          RetainedPosition retained;
                          AnchorVerifier   verifier{
                              provider,
                              retained,
                                {.maxAnchorAge = std::chrono::seconds{60}, .verificationTime = at(1030)}
                          };
                          checks.expect(run(verifier, log).age == AgeStatus::Fresh, "within T");
                      }
                      {
                          RetainedPosition retained;
                          AnchorVerifier   verifier{
                              provider,
                              retained,
                                {.maxAnchorAge = std::chrono::seconds{60}, .verificationTime = at(1100)}
                          };
                          const auto report = run(verifier, log);
                          checks.expect(report.age == AgeStatus::Stale && report.verdict == Verdict::Anchored && report.anchoredThrough == std::uint64_t{3},
                                        "older than T: stale, but coverage up to the anchor stays");
                      }
                      {
                          RetainedPosition retained;
                          AnchorVerifier   verifier{provider, retained, {.maxAnchorAge = std::chrono::seconds{60}}};
                          checks.expect(run(verifier, log).age == AgeStatus::Unknown, "no verification time: age unknown");
                      }
                      {
                          RetainedPosition retained;
                          AnchorVerifier   verifier{provider, retained};
                          checks.expect(run(verifier, log).age == AgeStatus::NotChecked, "no declared bound: not checked");
                      }
                      checks.raise();
                  })
            .Then("a trim at the anchor is anchored from the trim digest, and a trim past it cannot be verified",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 4);
                      const auto below = verifier.verify(streamName, log.records(3), {.afterSequence = 3, .afterDigest = log.digestAt(3)});
                      checks.expect(below.verdict == Verdict::Anchored && below.firstRetained == 4 && below.anchoredThrough == std::uint64_t{4},
                                    "trim at 3, anchor at 4");
                      const auto equal = verifier.verify(streamName, log.records(4), {.afterSequence = 4, .afterDigest = log.digestAt(4)});
                      checks.expect(equal.verdict == Verdict::Anchored && equal.anchoredThrough == std::uint64_t{4} && equal.unanchoredFrom == std::uint64_t{5},
                                    "trim at the anchor: the trim digest is compared directly");
                      const auto past = verifier.verify(streamName, log.records(5), {.afterSequence = 5, .afterDigest = log.digestAt(5)});
                      checks.expect(past.verdict == Verdict::CannotVerify && past.cause == VerdictCause::TrimPastAnchor, "H_p can no longer be recomputed");
                      const auto wrong = verifier.verify(streamName, {}, {.afterSequence = 4, .afterDigest = log.digestAt(3)});
                      checks.expect(wrong.verdict == Verdict::Altered && wrong.cause == VerdictCause::AnchorDigestMismatch,
                                    "a trim digest that is not the anchor's is Altered");
                      const auto chained = verifier.verify(streamName, log.records(4), {.afterSequence = 4, .afterDigest = log.digestAt(3)});
                      checks.expect(chained.verdict == Verdict::Inconsistent, "records that do not chain from the trim digest are Inconsistent");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register retainedPosition{
    "The retained position detects a rollback that log and anchor suffered together",
    "unit",
    [] {
        return speclab::Test("audit-anchor-retained-position")
            .Then("an old log restored with its old anchor is reported rolled back",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 3);
                      const auto oldState = provider.snapshot();
                      (void)anchorAt(provider, log, 6);
                      const auto first = run(verifier, log);
                      checks.expect(first.verdict == Verdict::Anchored && first.anchoredThrough == std::uint64_t{6}, "anchored through 6 before the rollback");
                      checks.expect(retained.anchor(streamName)->position == 6 && retained.head("witness-1") == std::uint64_t{2},
                                    "the verified position is retained");

                      provider.restore(oldState);
                      const auto restored = run(verifier, log, 0, 3);
                      checks.expect(restored.verdict == Verdict::RolledBack && restored.retained == RetainedOutcome::RolledBack,
                                    "rolled back, not anchored through 3");
                      checks.expect(restored.cause == VerdictCause::ProviderHeadBelowRetained, "the provider head is below the retained head");
                      checks.expect(!restored.rollbackNotExcluded() && !restored.anchoredThrough.has_value(), "never an unqualified pass");
                      checks.expect(retained.anchor(streamName)->position == 6 && retained.head("witness-1") == std::uint64_t{2},
                                    "a reported rollback changes nothing retained");
                      checks.raise();
                  })
            .Then("without a retained position the same restore is anchored, with rollback not excluded stated",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      (void)anchorAt(provider, log, 3);
                      RetainedPosition fresh;
                      AnchorVerifier   verifier{provider, fresh};
                      const auto       report = run(verifier, log, 0, 3);
                      checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == std::uint64_t{3} && report.rollbackNotExcluded(),
                                    "Anchored up to the old position, with rollback not excluded");
                      checks.raise();
                  })
            .Then("a stream anchor below the retained anchor is a rollback even when the head did not fall",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 6);
                      (void)run(verifier, log);
                      Anchor lower   = std::get<Anchor>(provider.latest(streamName));
                      lower.position = 3;
                      lower.digest   = log.digestAt(3);
                      provider.inject(lower);
                      provider.inject(Anchor{.anchorFormat     = 1,
                                             .canonicalVersion = 1,
                                             .streamId         = "pad/stream",
                                             .position         = 1,
                                             .digest           = {},
                                             .providerId       = "witness-1",
                                             .counter          = 9});
                      const auto report = run(verifier, log, 0, 3);
                      checks.expect(report.verdict == Verdict::RolledBack && report.cause == VerdictCause::StreamAnchorBelowRetained,
                                    "the stream anchor is below the retained one");
                      checks.raise();
                  })
            .Then("an idle stream whose counter is below the head is not a rollback",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(3);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 3);
                      (void)run(verifier, log);
                      provider.inject(Anchor{.anchorFormat     = 1,
                                             .canonicalVersion = 1,
                                             .streamId         = "busy/stream",
                                             .position         = 5,
                                             .digest           = {},
                                             .providerId       = "witness-1",
                                             .counter          = 40});
                      const auto report = run(verifier, log);
                      checks.expect(report.verdict == Verdict::Anchored && report.retained == RetainedOutcome::Passed && !report.rollbackNotExcluded(),
                                    "Anchored with the retained position passing");
                      checks.raise();
                  })
            .Then("a stream that vanished without a retirement is a rollback, and a retirement undone is too",
                  [] {
                      speclab::core::Checks checks;
                      const Log             log = writeLog(4);
                      {
                          InMemoryAnchorProvider provider{"witness-1"};
                          RetainedPosition       retained;
                          AnchorVerifier         verifier{provider, retained};
                          (void)anchorAt(provider, log, 4);
                          (void)run(verifier, log);
                          provider.erase(streamName);
                          const auto report = run(verifier, log);
                          checks.expect(report.verdict == Verdict::RolledBack && report.cause == VerdictCause::StreamMissing,
                                        "stream missing without a retirement");
                      }
                      {
                          InMemoryAnchorProvider provider{"witness-1"};
                          RetainedPosition       retained;
                          AnchorVerifier         verifier{provider, retained};
                          (void)anchorAt(provider, log, 4);
                          const auto before = provider.snapshot();
                          (void)provider.retire(streamName, 4);
                          const auto retiredReport = verifier.verify(streamName, {});
                          checks.expect(retiredReport.verdict == Verdict::Retired && retained.anchor(streamName)->retired,
                                        "a retired stream with no records is Retired, and retained as retired");
                          provider.restore(before);
                          const auto undone = run(verifier, log);
                          checks.expect(undone.verdict == Verdict::RolledBack, "an anchor back where a retirement was is a rollback");
                      }
                      checks.raise();
                  })
            .Then("a retired stream whose records remain is still checked against its final anchor and remembered as retired",
                  [] {
                      speclab::core::Checks checks;
                      const Log             log = writeLog(4);
                      {
                          InMemoryAnchorProvider provider{"witness-1"};
                          RetainedPosition       retained;
                          AnchorVerifier         verifier{provider, retained};
                          (void)anchorAt(provider, log, 4);
                          (void)provider.retire(streamName, 4);
                          const auto forged = run(verifier, rewriteWithRecompute(log, 2));
                          checks.expect(forged.verdict == Verdict::Altered && forged.cause == VerdictCause::AnchorDigestMismatch,
                                        "a rewrite after retirement is Altered");
                          const auto intact = run(verifier, log);
                          checks.expect(intact.verdict == Verdict::Anchored && intact.anchoredThrough == std::uint64_t{4},
                                        "the intact log still verifies against the final anchor");
                      }
                      {
                          InMemoryAnchorProvider provider{"witness-1"};
                          RetainedPosition       retained;
                          AnchorVerifier         verifier{provider, retained};
                          (void)anchorAt(provider, log, 4);
                          const auto before = provider.snapshot();
                          (void)provider.retire(streamName, 4);
                          (void)run(verifier, log);
                          checks.expect(retained.anchor(streamName)->retired, "retirement is retained although records remain");
                          provider.restore(before);
                          const auto undone = run(verifier, log);
                          checks.expect(undone.verdict == Verdict::RolledBack, "an anchor back where a retirement was is a rollback, records or not");
                      }
                      checks.raise();
                  })
            .Then("the retained checkpoint catches a history rewritten and re-anchored further on",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 3);
                      (void)run(verifier, log);
                      // The adversary rewrites record 2 and, holding the authority to advance, re-anchors past the checkpoint.
                      const Log forged = rewriteWithRecompute(log, 2);
                      (void)provider.advance(makeAnchorClaim(streamName, 5, forged.digestAt(5)));
                      const auto report = run(verifier, forged);
                      checks.expect(report.verdict == Verdict::Altered && report.cause == VerdictCause::RetainedCheckpointMismatch,
                                    "Altered at the retained checkpoint, not a rollback");
                      checks.raise();
                  })
            .Then("a log that ends before the retained checkpoint is Incomplete",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 5);
                      (void)run(verifier, log);
                      provider.setAvailable(false);
                      const auto report = run(verifier, log, 0, 4);
                      checks.expect(report.verdict == Verdict::Incomplete && report.cause == VerdictCause::LogEndsBeforeRetained
                                        && report.retained == RetainedOutcome::NotChecked,
                                    "the retained checkpoint protects the prefix even while the provider is down");
                      checks.raise();
                  })
            .Then("a trim past the retained checkpoint ends its protection, and the report says so",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(6);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 3);
                      (void)run(verifier, log);
                      (void)anchorAt(provider, log, 6);
                      const auto report = verifier.verify(streamName, log.records(4), {.afterSequence = 4, .afterDigest = log.digestAt(4)});
                      checks.expect(report.verdict == Verdict::Anchored && report.retained == RetainedOutcome::CheckpointTrimmed,
                                    "Anchored, with the checkpoint no longer applying");
                      checks.raise();
                  })
            .Then("only a clean verification raises the retained position, and values never fall",
                  [] {
                      speclab::core::Checks checks;
                      RetainedPosition      retained;
                      retained.raiseHead("w", 9);
                      retained.raiseHead("w", 4);
                      retained.raiseAnchor("s", {.position = 5, .digest = {}, .counter = 7, .retired = false});
                      retained.raiseAnchor("s", {.position = 3, .digest = {}, .counter = 9, .retired = false});
                      Sha256Digest other{};
                      other[0] = 1;
                      retained.raiseAnchor("s", {.position = 5, .digest = other, .counter = 99, .retired = true});
                      checks.expect(retained.head("w") == std::uint64_t{9}, "a lower head is ignored");
                      checks.expect(retained.anchor("s")->position == 5 && retained.anchor("s")->counter == 7 && !retained.anchor("s")->retired,
                                    "a lower position or a diverging digest at the same position is ignored");
                      retained.raiseAnchor("s", {.position = 5, .digest = {}, .counter = 8, .retired = true});
                      checks.expect(retained.anchor("s")->counter == 8 && retained.anchor("s")->retired, "the counter and the retired flag only rise");
                      checks.raise();
                  })
            .Then("a conflict between the provider's answers is reported, not resolved",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(4);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 4);
                      // A second answer for the same stream and position, with a different digest: only a faulty provider gives one.
                      class Divergent final : public AnchorProvider {
                      public:
                          explicit Divergent(InMemoryAnchorProvider& inner) : base(inner) {}
                          [[nodiscard]] AdvanceAnswer advance(const AnchorClaim& claim) override {
                              return base.advance(claim);
                          }
                          [[nodiscard]] RetireAnswer retire(std::string_view id, std::uint64_t position) override {
                              return base.retire(id, position);
                          }
                          [[nodiscard]] LatestAnswer latest(std::string_view id) override {
                              return base.latest(id);
                          }
                          [[nodiscard]] StreamsAnswer streams() override {
                              auto listing                                       = base.streams();
                              std::get<ProviderListing>(listing).entries.front() = [&] {
                                  Anchor changed     = std::get<Anchor>(std::get<ProviderListing>(listing).entries.front());
                                  changed.digest[0] ^= 0xFF;
                                  return changed;
                              }();
                              return listing;
                          }

                      private:
                          InMemoryAnchorProvider& base;
                      } faulty{provider};
                      AnchorVerifier faultyVerifier{faulty, retained};
                      const auto     report = run(faultyVerifier, log);
                      checks.expect(report.verdict == Verdict::Conflict && report.cause == VerdictCause::AnchorConflict,
                                    "Conflict, never a tie resolved by choosing one anchor");
                      checks.raise();
                  })
            .Then("streams the log lacks are reported: anchored ones Incomplete, retired ones Retired",
                  [] {
                      speclab::core::Checks  checks;
                      const Log              log = writeLog(3);
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      (void)anchorAt(provider, log, 3);
                      (void)provider.advance(AnchorClaim{.streamId = "gone/stream", .position = 2, .digest = log.digestAt(2)});
                      (void)provider.advance(AnchorClaim{.streamId = "aged/stream", .position = 2, .digest = log.digestAt(2)});
                      (void)provider.retire("aged/stream", 2);
                      const std::array<std::string, 1> present{std::string{streamName}};
                      const auto                       reports = verifier.verifyUnlisted(present);
                      checks.expect(reports.size() == 2, "two streams are not in the log");
                      for (const auto& report : reports) {
                          if (report.streamId == "gone/stream")
                              checks.expect(report.verdict == Verdict::Incomplete && report.lastPresent == 0,
                                            "an anchored stream with no record is Incomplete, m = 0");
                          else
                              checks.expect(report.verdict == Verdict::Retired, "an aged-out stream is Retired, not a finding");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

// NOLINTEND(hicpp-signed-bitwise)
}  // namespace
