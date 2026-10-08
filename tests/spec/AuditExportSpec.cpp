/** @brief Audit export replay preserves library reports, boundaries, inputs and refusal semantics. */
import std;
import speclab;
import mddlog.core.auditevent;
import mddlog.adapter.sha256;
import mddlog.adapter.auditprojection;
import mddlog.adapter.auditstore;

#include "../framework/AuditLogRig.hpp"

namespace {
using namespace mddlog::adapter;
using namespace mddlog::spec::auditlog;

[[nodiscard]] AuditEvidence fixture() {
    auto rig = makeRig();
    if (!rig->start() || rig->feed("export/producer", 1, 4) != 4)
        throw std::runtime_error("export fixture failed");
    rig->sink->close();
    const auto inventory = rig->medium.segments();
    const auto residual  = std::array<std::uint8_t, 3>{0xff, 0x00, 0x81};
    if (!inventory || rig->medium.append(inventory->back().segment, residual).status != AppendStatus::Written)
        throw std::runtime_error("residual fixture failed");
    VerifierConfig config;
    config.verificationTime     = mddlog::core::RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{-1}});
    config.maxAnchorAge         = std::chrono::seconds{10};
    auto evidence               = captureAuditEvidence(rig->medium, "test/reference", config);
    evidence.provider           = std::get<ProviderListing>(rig->provider.streams());
    evidence.providerProvenance = "test independent authority";
    RetainedPosition referencePosition;
    const auto       reference = LogVerifier(rig->medium, rig->provider, referencePosition, config).verify();
    EvidenceMedium   medium(evidence);
    EvidenceProvider provider(evidence.provider);
    RetainedPosition position;
    const auto       captured = LogVerifier(medium, provider, position, config).verify();
    mddlog::spec::compareAuditReports(reference, captured);
    if (referencePosition.allHeads() != position.allHeads() || referencePosition.allAnchors() != position.allAnchors())
        throw std::runtime_error("captured checkpoints differ");
    return evidence;
}

const speclab::Register roundTrip{
    "Evidence replay reproduces coverage and boundaries using original bytes",
    "unit",
    [] {
        return speclab::Test("audit-export-roundtrip")
            .Then("report, checkpoint, residual bytes, negative time and age bound survive export/import",
                  [] {
                      speclab::core::Checks checks;
                      const auto            original = fixture();
                      const auto            encoded  = encodeAuditEvidence(original);
                      const auto            imported = decodeAuditEvidence(encoded);
                      checks.expect(encodeAuditEvidence(imported) == encoded, "deterministic byte-preserving roundtrip");
                      checks.expect(imported.verification.verificationTime.value().time_since_epoch().count() == -1
                                        && imported.verification.maxAnchorAge == original.verification.maxAnchorAge,
                                    "verification time and age preserved");
                      EvidenceMedium   first(original);
                      EvidenceMedium   second(imported);
                      EvidenceProvider provider(original.provider);
                      EvidenceProvider replayProvider(imported.provider);
                      auto             before   = original.retained;
                      auto             after    = imported.retained;
                      const auto       expected = LogVerifier(first, provider, before, original.verification).verify();
                      const auto       actual   = LogVerifier(second, replayProvider, after, imported.verification).verify();
                      mddlog::spec::compareAuditReports(expected, actual);
                      checks.expect(before.allHeads() == after.allHeads() && before.allAnchors() == after.allAnchors(), "candidate positions identical");
                      const auto image = LogImage::read(second);
                      checks.expect(auditToolExit(actual, image) == AuditToolExit::Partial, "residual region prevents complete-coverage exit");
                      checks.expect(!after.allAnchors().empty(), "partial coverage still retains verified anchors");
                      for (const auto& [id, retained] : after.allAnchors()) {
                          const auto* stream = actual.find(id);
                          checks.expect(stream != nullptr && stream->report.anchor.has_value()
                                            && (stream->report.verdict == Verdict::Anchored || stream->report.verdict == Verdict::Retired)
                                            && retained.position == stream->report.anchor->position && retained.digest == stream->report.anchor->digest,
                                        "partial candidate contains only verified anchored positions");
                      }
                      const auto projection = auditProjectionJson(imported, actual, image, "accepted-embedded-assumption", "export/producer");
                      checks.expect(projection.contains("\"complete\":false") && projection.contains("\"trailing\":{") && projection.contains("canonicalHex"),
                                    "filtered projection explicitly partial with full report");
                      checks.expect(!second.reclaim(1) && second.sync(1, 0) == SyncAnswer::Unsupported, "replay medium refuses writes");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register refusals{"Evidence decoder rejects unsupported, truncated, duplicate and over-budget archives", "unit", [] {
                                     return speclab::Test("audit-export-refusals")
                                         .Then("malformed package never becomes a journal or silently enlarges budgets",
                                               [] {
                                                   speclab::core::Checks checks;
                                                   const auto            evidence = fixture();
                                                   const auto            encoded  = encodeAuditEvidence(evidence);
                                                   const auto            refused  = [](std::span<const std::uint8_t> bytes,
                                                                           AuditResourceLimits           limits  = {},
                                                                           std::size_t                   maximum = defaultEvidenceBytes) {
                                                       try {
                                                           (void)decodeAuditEvidence(bytes, limits, maximum);
                                                           return false;
                                                       } catch (const std::exception&) {
                                                           return true;
                                                       }
                                                   };
                                                   for (const auto size : {std::size_t{0}, std::size_t{8}, encoded.size() - 1, encoded.size() / 2})
                                                       checks.expect(refused(std::span(encoded).first(size)), "truncation refused");
                                                   auto unknown  = encoded;
                                                   unknown.at(8) = 2;
                                                   checks.expect(refused(unknown), "unknown schema refused");
                                                   auto damaged                    = encoded;
                                                   damaged.at(damaged.size() / 2) ^= std::uint8_t{1};
                                                   checks.expect(refused(damaged), "checksum mismatch refused");
                                                   auto extended = encoded;
                                                   extended.push_back(0);
                                                   checks.expect(refused(extended), "trailing archive bytes refused");
                                                   checks.expect(refused(encoded, {}, encoded.size() - 1), "archive byte budget refused");
                                                   AuditResourceLimits small;
                                                   small.maxSegments = 1;
                                                   checks.expect(refused(encoded, small), "embedded profile cannot raise local limits");
                                                   auto duplicate = evidence;
                                                   duplicate.segments.push_back(duplicate.segments.front());
                                                   bool encodeRefused = false;
                                                   try {
                                                       (void)encodeAuditEvidence(duplicate);
                                                   } catch (const std::exception&) {
                                                       encodeRefused = true;
                                                   }
                                                   checks.expect(encodeRefused, "duplicate references rejected by writer and reader");
                                                   const std::string               text = R"({"schema":"mddlog.audit.projection","version":1})";
                                                   const std::vector<std::uint8_t> projection(text.begin(), text.end());
                                                   checks.expect(refused(projection), "readable projection never treated as evidence");
                                                   checks.raise();
                                               })
                                         .Execute();
                                 }};

const speclab::Register hostileCounts{
    "Evidence counters and metadata refuse hostile short payloads with valid checksums",
    "unit",
    [] {
        return speclab::Test("audit-export-hostile-counts")
            .Then("head and anchor counters are bounded before records are decoded and oversized metadata names its field",
                  [] {
                      speclab::core::Checks checks;
                      const AuditEvidence   empty;
                      const auto            encoded = encodeAuditEvidence(empty);
                      // An empty v1 package ends with three u64 counts: heads, anchors, segments.
                      constexpr std::size_t countFields = 3;
                      for (std::size_t field = 0; field < countFields - 1; ++field) {
                          for (const auto count : {std::uint64_t{1},
                                                   static_cast<std::uint64_t>(empty.verification.resources.maxProviderEntries) + 1,
                                                   std::numeric_limits<std::uint64_t>::max()}) {
                              const auto                offset = encoded.size() - sha256DigestSize - ((countFields - field) * sizeof(std::uint64_t));
                              std::vector<std::uint8_t> hostile(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(offset));
                              for (unsigned byte = 0; byte < sizeof(count); ++byte)
                                  hostile.push_back(static_cast<std::uint8_t>(count >> (byte * std::numeric_limits<std::uint8_t>::digits)));
                              const auto digest = sha256(hostile);
                              hostile.insert(hostile.end(), digest.begin(), digest.end());
                              bool refused = false;
                              try {
                                  (void)decodeAuditEvidence(hostile);
                              } catch (const std::exception&) {
                                  refused = true;
                              }
                              checks.expect(refused, "valid checksum cannot legitimize huge counts or missing count payloads");
                          }
                      }
                      for (const bool source : {true, false}) {
                          auto  oversized = empty;
                          auto& field     = source ? oversized.source : oversized.providerProvenance;
                          field.assign(oversized.verification.resources.maxProviderTextBytes + 1, 'x');
                          bool named = false;
                          try {
                              (void)encodeAuditEvidence(oversized);
                          } catch (const std::length_error& error) {
                              named = std::string_view(error.what()).contains(source ? "source exceeds" : "providerProvenance exceeds");
                          }
                          checks.expect(named, "oversized field has an actionable diagnostic");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register trustAndBudgets{"Trust and work budgets remain explicit during evidence verification", "unit", [] {
                                            return speclab::Test("audit-export-trust-budgets")
                                                .Then("untrusted provider is unavailable, tampering is adverse, and work refusal leaves positions unchanged",
                                                      [] {
                                                          speclab::core::Checks                checks;
                                                          auto                                 evidence = fixture();
                                                          EvidenceMedium                       medium(evidence);
                                                          const std::optional<ProviderListing> absent;
                                                          EvidenceProvider                     untrusted(absent);
                                                          RetainedPosition                     position;
                                                          const auto                           noTrust = LogVerifier(medium, untrusted, position).verify();
                                                          checks.expect(std::ranges::all_of(noTrust.streams,
                                                                                            [](const auto& stream) {
                                                                                                return stream.report.verdict == Verdict::AnchorUnavailable;
                                                                                            }),
                                                                        "embedded data alone confers no trust");
                                                          checks.expect(position.empty(), "no retained state from untrusted anchors");
                                                          EvidenceProvider accepted(evidence.provider);
                                                          auto             tiny     = evidence.verification;
                                                          tiny.resources.maxRecords = 1;
                                                          const auto bounded        = LogVerifier(medium, accepted, position, tiny).verify();
                                                          checks.expect(bounded.resourceIssue == AuditResourceIssue::Records && bounded.streams.empty()
                                                                            && position.empty(),
                                                                        "work refusal discards verdicts and candidates");
                                                          const auto changed = std::ranges::find_if(evidence.segments, [](const auto& segment) {
                                                              const std::string bytes(segment.bytes.begin(), segment.bytes.end());
                                                              return bytes.contains("therapy.rate.set");
                                                          });
                                                          if (changed == evidence.segments.end())
                                                              throw std::runtime_error("event fixture missing");
                                                          const std::string bytes(changed->bytes.begin(), changed->bytes.end());
                                                          changed->bytes.at(bytes.find("therapy.rate.set")) = 'X';
                                                          const auto report                                 = LogVerifier(medium, accepted, position).verify();
                                                          const auto image                                  = LogImage::read(medium);
                                                          checks.expect(auditToolExit(report, image) == AuditToolExit::Adverse,
                                                                        "canonical tampering reports adverse finding even with accepted anchors");
                                                          checks.raise();
                                                      })
                                                .Execute();
                                        }};
}  // namespace
