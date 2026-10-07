/** @brief Resource limits reject hostile inventory before reads and preserve verification semantics. */
import std;
import speclab;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

namespace {
using namespace mddlog::adapter;

class ObservedMedium final : public StorageMedium {
public:
    InMemoryStorageMedium                   inner{16};
    std::optional<std::vector<SegmentInfo>> forged;
    std::size_t                             reads          = 0;
    std::uint64_t                           largestRead    = 0;
    bool                                    failAllocation = false;
    OpenAnswer                              open(const SegmentOpening& opening) override {
        return inner.open(opening);
    }
    AppendAnswer append(SegmentRef segment, std::span<const std::uint8_t> bytes) override {
        return inner.append(segment, bytes);
    }
    SyncAnswer sync(SegmentRef segment, std::uint64_t offset) override {
        return inner.sync(segment, offset);
    }
    std::optional<std::vector<std::uint8_t>> read(SegmentRef segment, std::uint64_t offset, std::uint64_t length) override {
        ++reads;
        largestRead = std::max(largestRead, length);
        if (failAllocation)
            throw std::bad_alloc{};
        return inner.read(segment, offset, length);
    }
    std::optional<std::vector<SegmentInfo>> segments() override {
        return forged ? forged : inner.segments();
    }
    bool reclaim(SegmentRef segment) override {
        return inner.reclaim(segment);
    }
};

class CountingProvider final : public AnchorProvider {
public:
    InMemoryAnchorProvider inner{"witness"};
    std::size_t            reads = 0;
    AdvanceAnswer          advance(const AnchorClaim& claim) override {
        return inner.advance(claim);
    }
    RetireAnswer retire(std::string_view id, std::uint64_t position) override {
        return inner.retire(id, position);
    }
    LatestAnswer latest(std::string_view id) override {
        ++reads;
        return inner.latest(id);
    }
    StreamsAnswer streams() override {
        ++reads;
        return inner.streams();
    }
};

void populate(ObservedMedium& medium, std::string_view id, std::size_t records = 3) {
    const auto opening = encodeSegmentOpening(id, 0, 1);
    const auto held    = medium.open({.streamId = id, .segmentIndex = 0, .firstSequence = 1, .bytes = opening});
    if (held.status != OpenStatus::Opened)
        throw std::runtime_error("fixture open");
    AuditChain chain{id};
    for (std::size_t sequence = 1; sequence <= records; ++sequence) {
        mddlog::core::AuditEvent event;
        if (!event.assign({.action = "resources.test", .target = "fixture"}, id, sequence).wasAdmitted())
            throw std::runtime_error("fixture event");
        const auto record = chain.append(event);
        if (!record || medium.append(held.segment, encodeRecordFrame(record->canonical.bytes(), record->digest)).status != AppendStatus::Written)
            throw std::runtime_error("fixture append");
    }
}

const speclab::Register inventoryLimits{
    "Audit resource inventory limits precede every byte allocation",
    "unit",
    [] {
        return speclab::Test("audit-resource-inventory")
            .Then("exact bounds pass and oversized or overflowing sizes never reach read",
                  [] {
                      speclab::core::Checks checks;
                      for (const auto issue : {AuditResourceIssue::Segments, AuditResourceIssue::SegmentBytes, AuditResourceIssue::TotalBytes}) {
                          ObservedMedium      medium;
                          AuditResourceLimits limits;
                          limits.maxSegments     = 1;
                          limits.maxSegmentBytes = 128;
                          limits.maxTotalBytes   = 128;
                          if (issue == AuditResourceIssue::Segments)
                              medium.forged = std::vector<SegmentInfo>{
                                  {1, 64},
                                  {2, 64}
                              };
                          if (issue == AuditResourceIssue::SegmentBytes)
                              medium.forged = std::vector<SegmentInfo>{
                                  {1, std::numeric_limits<std::uint64_t>::max()}
                              };
                          if (issue == AuditResourceIssue::TotalBytes) {
                              limits.maxSegments = 2;
                              medium.forged      = std::vector<SegmentInfo>{
                                  {1, 128},
                                  {2,   1}
                              };
                          }
                          const auto image = LogImage::read(medium, limits);
                          checks.expect(image.resourceIssue() == issue && image.unreadable() && image.streams().empty() && medium.reads == 0,
                                        "inventory refused before data reads");
                          const auto startup = checkMediumAtStart(medium, limits);
                          checks.expect(startup.resourceIssue == issue && !startup.mediumReadable && medium.reads == 0, "startup shares the bounds");
                          const auto stored = readStoredStream(medium, "producer", limits);
                          checks.expect(stored.resourceIssue() == issue && !stored.complete() && medium.reads == 0, "single-stream reader shares the bounds");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register boundedRecords{
    "Audit resource chunking keeps verdicts and refuses excess records",
    "unit",
    [] {
        return speclab::Test("audit-resource-chunks-records")
            .Then("bounded and reference readers agree while exact limits remain inclusive",
                  [] {
                      speclab::core::Checks checks;
                      ObservedMedium        medium;
                      populate(medium, "bounded");
                      InMemoryAnchorProvider provider{"witness"};
                      RetainedPosition       first;
                      const auto             reference = LogVerifier{medium, provider, first}.verify();
                      AuditResourceLimits    limits;
                      limits.readChunkBytes = 7;
                      limits.maxRecords     = 3;
                      limits.maxStreams     = 1;
                      limits.maxSegments    = 1;
                      const auto listing    = medium.segments();
                      checks.expect(listing.has_value(), "fixture inventory");
                      checks.raise();
                      limits.maxSegmentBytes = listing->at(0).size;
                      limits.maxTotalBytes   = listing->at(0).size;
                      limits.maxReadBytes    = listing->at(0).size;
                      RetainedPosition second;
                      medium.largestRead = 0;
                      const auto bounded = LogVerifier{medium, provider, second, {.resources = limits}}.verify();
                      checks.expect(bounded.resourceIssue == AuditResourceIssue::None && medium.largestRead <= 7
                                        && bounded.streams.size() == reference.streams.size(),
                                    "exact volume and record bounds pass with small requests");
                      checks.expect(bounded.streams.at(0).report.verdict == reference.streams.at(0).report.verdict
                                        && bounded.streams.at(0).report.lastPresent == reference.streams.at(0).report.lastPresent
                                        && bounded.notes.size() == reference.notes.size(),
                                    "verdict and boundaries preserved");
                      limits.maxRecords   = 2;
                      const auto exceeded = LogVerifier{medium, provider, second, {.resources = limits}}.verify();
                      checks.expect(exceeded.resourceIssue == AuditResourceIssue::Records && exceeded.streams.empty() && exceeded.unlisted.empty(),
                                    "no apparently complete verdict after record limit");
                      limits.maxRecords    = 3;
                      limits.maxReadBytes -= 1;
                      checks.expect(LogImage::read(medium, limits).resourceIssue() == AuditResourceIssue::ReadBytes, "cumulative read bound explicit");
                      medium.failAllocation = true;
                      checks.expect(LogImage::read(medium).resourceIssue() == AuditResourceIssue::MemoryUnavailable, "allocation failure explicit");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register boundedProvider{
    "Audit provider budgets preserve the retained checkpoint on incomplete verification",
    "unit",
    [] {
        return speclab::Test("audit-resource-provider-checkpoint")
            .Then("entry and call limits include retired history",
                  [] {
                      speclab::core::Checks  checks;
                      ObservedMedium         medium;
                      InMemoryAnchorProvider provider{"witness"};
                      for (const std::string_view id : {"history/1", "history/2"}) {
                          checks.expect(std::holds_alternative<AnchorStamp>(provider.advance(makeAnchorClaim(id, 1, chainInitialValue))), "anchor history");
                          checks.expect(std::holds_alternative<AnchorStamp>(provider.retire(id, 1)), "retired history");
                      }
                      RetainedPosition    retained;
                      AuditResourceLimits limits;
                      limits.maxProviderEntries = 1;
                      const auto exceeded       = LogVerifier{medium, provider, retained, {.resources = limits}}.verify();
                      checks.expect(exceeded.resourceIssue == AuditResourceIssue::ProviderEntries && exceeded.unlisted.empty() && retained.empty(),
                                    "retired entries count and checkpoint stays unchanged");
                      limits.maxProviderEntries = 2;
                      limits.maxProviderCalls   = 1;
                      const auto calls          = LogVerifier{medium, provider, retained, {.resources = limits}}.verify();
                      checks.expect(calls.resourceIssue == AuditResourceIssue::ProviderCalls && calls.providerCalls == 1 && retained.empty(),
                                    "call limit is exact");
                      limits.maxProviderCalls = 32;
                      const auto complete     = LogVerifier{medium, provider, retained, {.resources = limits}}.verify();
                      checks.expect(complete.resourceIssue == AuditResourceIssue::None && complete.unlisted.size() == 2 && retained.allAnchors().size() == 2,
                                    "exact provider entry bound succeeds");
                      RetainedPosition heads;
                      heads.raiseHead("other/1", 1);
                      heads.raiseHead("other/2", 1);
                      const auto headLimit = LogVerifier{medium, provider, heads, {.resources = limits}}.verify();
                      checks.expect(headLimit.resourceIssue == AuditResourceIssue::ProviderEntries && heads.allHeads().size() == 2
                                        && heads.allAnchors().empty(),
                                    "provider head growth cannot overflow a full checkpoint");
                      InMemoryAnchorProvider largeText{std::string(1025, 'x')};
                      checks.expect(std::holds_alternative<AnchorStamp>(largeText.advance(makeAnchorClaim("history", 1, chainInitialValue))),
                                    "large identity fixture");
                      RetainedPosition textCheckpoint;
                      const auto       textLimit = LogVerifier{medium, largeText, textCheckpoint}.verify();
                      checks.expect(textLimit.resourceIssue == AuditResourceIssue::ProviderTextBytes && textCheckpoint.empty(),
                                    "text checked before reader copies");
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace

namespace {
const speclab::Register consumerHistory{
    "Audit consumer profiles bound ended identities and reject startup before mutation",
    "unit",
    [] {
        return speclab::Test("audit-resource-consumer-history")
            .Then("invalid profiles and full histories never write a new origin",
                  [] {
                      speclab::core::Checks checks;
                      ObservedMedium        medium;
                      StorageConfig         config;
                      config.segmentSize          = 2048;
                      config.segmentCount         = 16;
                      config.maxProducerStreams   = 1;
                      config.resources.maxStreams = 2;
                      config.ledger               = LedgerConfig{.streamId = "ledger/1"};
                      auto made                   = PersistingAuditSink::create(medium, config);
                      checks.expect(made.has_value(), "profile starts");
                      checks.raise();
                      mddlog::core::AuditEvent event;
                      checks.expect(event.assign({.action = "test.event", .target = "fixture"}, "producer/1", 1).wasAdmitted(), "fixture event");
                      checks.expect((*made)->accept(event) && (*made)->closeStream("producer/1"), "one lifetime identity accepted and closed");
                      checks.expect(event.assign({.action = "test.event", .target = "fixture"}, "producer/2", 1).wasAdmitted(), "next event");
                      checks.expect(!(*made)->accept(event) && (*made)->health().resourceIssue == AuditResourceIssue::Streams,
                                    "ended identity still consumes capacity");
                      (*made)->close();
                      const auto          inventory = medium.segments();
                      AuditResourceLimits narrow;
                      narrow.maxStreams   = 1;
                      const auto existing = LogAnalysis::read(medium);
                      checks.expect(recoverChainState(existing, nullptr, narrow).resourceIssue == AuditResourceIssue::Streams,
                                    "recovery validates its own profile even for a preloaded image");
                      narrow.maxStreams       = 2;
                      narrow.maxProviderCalls = 0;
                      checks.expect(recoverChainState(existing, nullptr, narrow).resourceIssue == AuditResourceIssue::InvalidLimits,
                                    "invalid recovery limits rejected without provider");
                      config.ledger      = LedgerConfig{.streamId = "ledger/2"};
                      const auto refused = PersistingAuditSink::create(medium, config);
                      checks.expect(!refused && refused.error() == StorageConfigError::ResourceLimit && medium.segments()->size() == inventory->size(),
                                    "history budget checked before writing origin");
                      config.resources.maxStreams = 3;
                      checks.expect(PersistingAuditSink::create(medium, config).has_value(), "exact history plus new ledger fits");
                      config.resources.maxReadBytes = 0;
                      const auto invalid            = PersistingAuditSink::create(medium, config);
                      checks.expect(!invalid && invalid.error() == StorageConfigError::InvalidResources, "zero work budget invalid");
                      checks.expect(ledgerReserveSegments(2048, std::numeric_limits<std::size_t>::max(), 1) == 0, "reserve arithmetic cannot wrap");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register faultBudget{
    "Audit recovery refuses an integrity inventory beyond the declared bound",
    "unit",
    [] {
        return speclab::Test("audit-resource-fault-budget")
            .Then("all faults fit at the limit and excess faults give no usable recovery",
                  [] {
                      speclab::core::Checks  checks;
                      ObservedMedium         medium;
                      InMemoryAnchorProvider provider{"witness"};
                      for (const std::string_view id : {"missing/1", "missing/2"})
                          checks.expect(std::holds_alternative<AnchorStamp>(provider.advance(makeAnchorClaim(id, 1, chainInitialValue))),
                                        "missing stream anchor");
                      const auto          log = LogAnalysis::read(medium);
                      AuditResourceLimits limits;
                      limits.maxIntegrityFaults = 2;
                      auto exact                = recoverChainState(log, &provider, limits);
                      checks.expect(exact.resourceIssue == AuditResourceIssue::None && exact.faults.size() == 2, "exact fault budget accepted");
                      limits.maxIntegrityFaults = 1;
                      auto excess               = recoverChainState(log, &provider, limits);
                      checks.expect(excess.resourceIssue == AuditResourceIssue::IntegrityFaults && excess.faults.empty() && excess.held.empty(),
                                    "no partial recovered state");
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace

namespace {
static_assert(!std::is_copy_constructible_v<AnchorVerifier> && !std::is_copy_assignable_v<AnchorVerifier>);
static_assert(!std::is_move_constructible_v<AnchorVerifier> && !std::is_move_assignable_v<AnchorVerifier>);
static_assert(!std::is_copy_constructible_v<LogVerifier> && !std::is_copy_assignable_v<LogVerifier>);
static_assert(!std::is_move_constructible_v<LogVerifier> && !std::is_move_assignable_v<LogVerifier>);

const speclab::Register verifierLifetime{
    "Audit resource verifier ownership and independent batches preserve their provider budget",
    "unit",
    [] {
        return speclab::Test("audit-resource-verifier-lifetime-budget")
            .Then("nontransferable verifiers construct in place and standalone batches reset explicitly",
                  [] {
                      speclab::core::Checks checks;
                      ObservedMedium        medium;
                      populate(medium, "stream/1");
                      populate(medium, "stream/2");
                      CountingProvider provider;
                      for (const std::string_view id : {"stream/1", "stream/2"}) {
                          const auto held = readStoredStream(medium, id);
                          checks.expect(std::holds_alternative<AnchorStamp>(provider.advance(makeAnchorClaim(id, 3, held.records().back().digest))),
                                        "anchored fixture");
                      }
                      const auto       held = readStoredStream(medium, "stream/1");
                      RetainedPosition retained;
                      VerifierConfig   config;
                      config.resources.maxProviderCalls = 2;
                      std::optional<AnchorVerifier> owned{std::in_place, provider, retained, config};
                      const auto                    verifyStream = [&](AnchorVerifier& verifier) {
                          return verifier.verify("stream/1", held.records(), {}, held.layout());
                      };
                      checks.expect(verifyStream(*owned).verdict == Verdict::Anchored, "constructs in optional without transfer");
                      checks.expect(verifyStream(*owned).cause == VerdictCause::ResourceLimit && owned->resourceIssue() == AuditResourceIssue::ProviderCalls,
                                    "one batch shares its call budget");
                      owned->resetBudget();
                      checks.expect(owned->resourceIssue() == AuditResourceIssue::None && verifyStream(*owned).verdict == Verdict::Anchored,
                                    "fresh standalone batch clears refusal and call count");
                      ResourceAnchorProvider batch{provider, config.resources};
                      auto                   shared       = AnchorVerifier::withSharedBudget(batch, retained, config);
                      const auto             beforeShared = provider.reads;
                      checks.expect(verifyStream(shared).verdict == Verdict::Anchored && batch.calls() == 2 && provider.reads - beforeShared == 2,
                                    "shared wrapper counts actual reads");
                      checks.expect(verifyStream(shared).cause == VerdictCause::ResourceLimit && batch.calls() == 2, "shared budget caps the entire batch");
                      shared.resetBudget();
                      checks.expect(batch.calls() == 0 && verifyStream(shared).verdict == Verdict::Anchored, "reset reaches the shared envelope");

                      config.resources.maxProviderCalls = 5;
                      std::optional<LogVerifier> whole{std::in_place, medium, provider, retained, config};
                      const auto                 verifyLog = [&](LogVerifier& verifier) {
                          const auto before = provider.reads;
                          const auto report = verifier.verify();
                          checks.expect(report.resourceIssue == AuditResourceIssue::None && report.streams.size() == 2 && report.providerCalls == 5
                                            && provider.reads - before == report.providerCalls,
                                        "one shared envelope counts actual reads across the complete log");
                          checks.expect(std::ranges::all_of(report.streams,
                                                            [](const auto& stream) {
                                                                return stream.report.verdict == Verdict::Anchored;
                                                            }),
                                        "anchored log remains complete");
                      };
                      verifyLog(*whole);
                      verifyLog(*whole);
                      RetainedPosition untouched;
                      config.resources.maxProviderCalls = 4;
                      const auto before                 = provider.reads;
                      const auto refused                = LogVerifier{medium, provider, untouched, config}.verify();
                      checks.expect(refused.resourceIssue == AuditResourceIssue::ProviderCalls && refused.providerCalls == 4 && provider.reads - before == 4
                                        && untouched.empty(),
                                    "global call refusal never commits a partial checkpoint");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register startupRefusal{"Audit resource startup refuses a session limit without inventing unreadable segments", "unit", [] {
                                           return speclab::Test("audit-resource-startup-refusal")
                                               .Then("prefix and final-segment passes stop immediately and retain only actual findings",
                                                     [] {
                                                         speclab::core::Checks checks;
                                                         ObservedMedium        medium;
                                                         populate(medium, "stream/1");
                                                         populate(medium, "stream/2");
                                                         AuditResourceLimits limits;
                                                         limits.maxReadBytes = 1;
                                                         const auto prefix   = checkMediumAtStart(medium, limits);
                                                         checks.expect(prefix.resourceIssue == AuditResourceIssue::ReadBytes && !prefix.mediumReadable
                                                                           && prefix.findings.empty() && prefix.segmentsExamined == 1 && medium.reads == 0,
                                                                       "a budget refusal is not an unreadable segment");
                                                         limits.maxReadBytes  = 0;
                                                         const auto inventory = medium.segments();
                                                         for (const auto& info : *inventory)
                                                             limits.maxReadBytes += std::min<std::uint64_t>(info.size, maxSegmentOpeningSize);
                                                         const auto final = checkMediumAtStart(medium, limits);
                                                         checks.expect(final.resourceIssue == AuditResourceIssue::ReadBytes && final.findings.empty()
                                                                           && medium.reads == 2,
                                                                       "final pass stops on its first budget refusal");
                                                         ObservedMedium                    malformed;
                                                         const std::array<std::uint8_t, 1> invalid{0};
                                                         checks.expect(malformed.open({.streamId = "bad", .bytes = invalid}).status == OpenStatus::Opened,
                                                                       "malformed fixture");
                                                         populate(malformed, "good");
                                                         limits.maxReadBytes = 1;
                                                         const auto actual   = checkMediumAtStart(malformed, limits);
                                                         checks.expect(actual.resourceIssue == AuditResourceIssue::ReadBytes && actual.findings.size() == 1
                                                                           && actual.findings.front().kind == RecoveryFindingKind::NoValidPreamble
                                                                           && malformed.reads == 1,
                                                                       "genuine prior finding remains distinct from refusal");
                                                         checks.raise();
                                                     })
                                               .Execute();
                                       }};
}  // namespace
