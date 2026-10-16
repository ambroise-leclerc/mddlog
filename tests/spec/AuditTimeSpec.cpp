/** @brief Host time quality and elapsed startup time survive audit persistence and expose tampering (#154, #155). */
import std;
import speclab;
import mddlog;
import mddlog.core.ring;
import mddlog.adapter.ringdrain;
import mddlog.adapter.logrecord;
import mddlog.adapter.transportconsumer;
import mddlog.core.auditbinding;
import mddlog.adapter.auditprojection;
import mddlog.adapter.witnesscodec;

#include "../framework/AuditLogRig.hpp"
#include "../framework/RecordingSink.hpp"

namespace {
using namespace mddlog::core;
using namespace mddlog::adapter;
using namespace std::chrono;

constexpr auto civil = sys_time<nanoseconds>{nanoseconds{-123}};
static_assert(RawTime::doubtful(civil).availability() == TimeAvailability::Doubtful);
static_assert(RawTime::doubtful(civil).value() == civil);

const speclab::Register persistedTime{
    "Host time quality and monotonic time survive persisted audit and evidence replay",
    "unit",
    [] {
        return speclab::Test("audit-host-time-persistence")
            .Then("three civil states and optional elapsed values survive ring, binding, persistence and export",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = mddlog::spec::auditlog::makeRig();
                      checks.expect(rig->start(), "sink started");
                      if (!rig->sink) {
                          checks.raise();
                          return;
                      }
                      AuditRing<4>                                    ring("time/boot-1");
                      const auto                                      description = AuditDescription::create({.action = "device.start"});
                      const auto                                      context     = AuditContext::create({.target = "device"});
                      AuditBinding                                    binding(ring, *description, *context);
                      const std::string                               longDetail(auditDetailCapacity + 10, 'x');
                      const std::array                                clocks{RawTime::available(civil), RawTime::doubtful(civil), RawTime::unavailable()};
                      const std::array<std::optional<nanoseconds>, 3> elapsed{std::nullopt, nanoseconds{0}, nanoseconds::max()};
                      for (std::size_t i = 0; i < clocks.size(); ++i) {
                          checks.expect(binding.record(AuditPhase::Executed, clocks[i], {.detail = longDetail, .monotonicTime = elapsed[i]}).wasAdmitted(),
                                        "admitted");
                      }
                      const auto view = ring.drain();
                      for (const auto& event : view.first()) {
                          const auto i = static_cast<std::size_t>(event.sequence() - 1);
                          checks.expect(event.monotonicTime() == elapsed[i] && event.detailTruncated(), "elapsed value is independent of detail truncation");
                          checks.expect(rig->sink->accept(event), "persisted");
                      }
                      rig->sink->close();
                      rig->medium.restart(InMemoryStorageMedium::Unconfirmed::Dropped);
                      auto evidence                          = captureAuditEvidence(rig->medium, "time-test");
                      evidence.verification.verificationTime = RawTime::doubtful(civil);
                      evidence.provider                      = std::get<ProviderListing>(rig->provider.streams());
                      auto archive                           = decodeAuditEvidence(encodeAuditEvidence(evidence));
                      checks.expect(archive.verification.verificationTime.availability() == TimeAvailability::Doubtful
                                        && archive.verification.verificationTime.value() == civil,
                                    "evidence preserves doubtful verification time");
                      EvidenceMedium   medium(archive);
                      EvidenceProvider provider(archive.provider);
                      const auto       image  = LogImage::read(medium);
                      const auto*      stream = image.find("time/boot-1");
                      checks.expect(stream && stream->records.size() == clocks.size(), "all persisted events reloaded");
                      if (stream) {
                          AuditChainVerifier chain("time/boot-1");
                          for (std::size_t i = 0; i < stream->records.size(); ++i) {
                              const auto& stored  = stream->records[i];
                              const auto  decoded = decodeCanonical(stored.bytes);
                              checks.expect(decoded.status == CanonicalReadStatus::Ok && decoded.record.monotonicTime == elapsed[i]
                                                && decoded.record.time.availability() == clocks[i].availability()
                                                && decoded.record.time.value() == clocks[i].value(),
                                            "exact time and quality round trip");
                              checks.expect(chain.check(stored.bytes, stored.digest) == ChainFinding::Ok, "original chain verifies");
                          }
                      }
                      RetainedPosition retained;
                      const auto       report     = LogVerifier(medium, provider, retained).verify();
                      const auto       projection = auditProjectionJson(archive, report, image, "test");
                      checks.expect(projection.contains("\"timeStatus\":\"doubtful\"") && projection.contains("\"timeNs\":-123")
                                        && projection.contains("\"monotonicTimeNs\":0") && projection.contains("\"monotonicTimeNs\":null"),
                                    "reader renders explicit states and elapsed value");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register timeTampering{
    "Audit chain commits to elapsed time and civil time quality",
    "unit",
    [] {
        return speclab::Test("audit-host-time-tampering")
            .Then("each time field change breaks the stored digest and malformed optional fields are refused",
                  [] {
                      speclab::core::Checks checks;
                      AuditEvent            event;
                      (void)event.assign({.time = RawTime::doubtful(civil), .action = "a", .target = "t", .monotonicTime = nanoseconds{42}}, "s", 1);
                      const auto encoded  = encodeCanonical(event);
                      const auto original = encoded->bytes();
                      const auto digest   = chainDigest(original, chainInitialValue);
                      // Header 2 + counted stream 3 + sequence 8 + category/phase 2 = time tag at 15.
                      auto quality   = std::vector<std::uint8_t>(original.begin(), original.end());
                      quality.at(15) = 1;
                      AuditChainVerifier civilVerifier("s");
                      checks.expect(decodeCanonical(quality).record.time.availability() == TimeAvailability::Available
                                        && civilVerifier.check(quality, digest) == ChainFinding::DigestMismatch,
                                    "changing only quality is detected");
                      auto elapsed   = std::vector<std::uint8_t>(original.begin(), original.end());
                      elapsed.back() = 43;
                      AuditChainVerifier monotonicVerifier("s");
                      checks.expect(decodeCanonical(elapsed).record.monotonicTime == nanoseconds{43}
                                        && monotonicVerifier.check(elapsed, digest) == ChainFinding::DigestMismatch,
                                    "changing only elapsed time is detected");
                      elapsed.at(elapsed.size() - 9) = 2;
                      checks.expect(decodeCanonical(elapsed).status == CanonicalReadStatus::Malformed, "invalid elapsed tag");
                      for (std::size_t cut = 1; cut <= 9; ++cut)
                          checks.expect(decodeCanonical(original.first(original.size() - cut)).status == CanonicalReadStatus::Malformed,
                                        "truncated elapsed value");
                      (void)event.assign({.action = "a", .target = "t", .monotonicTime = nanoseconds::min()}, "s", 1);
                      checks.expect(decodeCanonical(encodeCanonical(event)->bytes()).record.monotonicTime == nanoseconds::min(),
                                    "signed representation preserved without host clock validation");
                      // The fixed buffer must fit every maximum field together with both clocks.
                      const std::string action(auditActionCapacity, 'a');
                      const std::string actor(auditActorCapacity, 'b');
                      const std::string target(auditTargetCapacity, 'c');
                      const std::string reference(auditReferenceCapacity, 'd');
                      const std::string correlation(auditCorrelationCapacity, 'e');
                      const std::string stream(auditStreamCapacity, 'f');
                      const std::string detail(auditDetailCapacity, 'g');
                      checks.expect(event
                                        .assign({.time           = RawTime::doubtful(civil),
                                                 .action         = action,
                                                 .actor          = actor,
                                                 .target         = target,
                                                 .requirementRef = reference,
                                                 .riskRef        = reference,
                                                 .correlationId  = correlation,
                                                 .sourceSequence = std::numeric_limits<std::uint64_t>::max(),
                                                 .detail         = detail,
                                                 .monotonicTime  = nanoseconds::max()},
                                                stream,
                                                1)
                                        .wasAdmitted(),
                                    "maximum fields admitted");
                      const auto maximum = encodeCanonical(event);
                      checks.expect(maximum && maximum->size() == canonicalMaxSize && decodeCanonical(maximum->bytes()).status == CanonicalReadStatus::Ok,
                                    "maximum v2 record fits and decodes");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register diagnosticTime{
    "Diagnostic text adapters retain doubtful civil time",
    "unit",
    [] {
        return speclab::Test("diagnostic-doubtful-time")
            .Then("both sink and transport consumers render doubt without replacing the timestamp",
                  [] {
                      speclab::core::Checks checks;
                      RingLog<1>            ring;
                      auto                  sink = std::make_shared<mddlog::spec::RecordingSink>();
                      RingSinkAdapter       adapter;
                      adapter.addRing(ring);
                      adapter.addSink(sink);
                      checks.expect(ring.tryWrite({.time = RawTime::doubtful(civil), .message = "clock jump"}).admission() == Admission::Written, "admitted");
                      checks.expect(adapter.drainOnce() == 1, "sink consumed");
                      const auto record = sink->records().front();
                      checks.expect(record.timeAvailable && record.timeDoubtful && record.timestamp == time_point_cast<LogRecord::TimePoint::duration>(civil)
                                        && record.getFormattedTimestamp().ends_with(" [doubtful]"),
                                    "text distinguishes doubtful time");
                      TransportConsumer consumer;
                      consumer.addRing(ring);
                      bool       copied = false;
                      const auto handle = consumer.addTransport([&](const LogRecord& value) {
                          copied = value.timeAvailable && value.timeDoubtful && value.timestamp == record.timestamp;
                      });
                      checks.expect(ring.tryWrite({.time = RawTime::doubtful(civil), .message = "again"}).admission() == Admission::Written, "admitted again");
                      (void)consumer.drainOnce();
                      checks.expect(copied, "transport preserves doubt");
                      (void)handle;
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register witnessTime{"Witness codec preserves doubtful acceptance time", "unit", [] {
                                        return speclab::Test("witness-doubtful-time")
                                            .Then("the current codec round trips all three tags, and legacy decoding refuses the new tag",
                                                  [] {
                                                      speclab::core::Checks checks;
                                                      for (auto clock : {RawTime::available(civil), RawTime::doubtful(civil), RawTime::unavailable()}) {
                                                          witness::Encoder encoder;
                                                          encoder.time(clock);
                                                          witness::Decoder decoder(encoder.bytes);
                                                          const auto       restored = decoder.time();
                                                          decoder.end();
                                                          checks.expect(restored.availability() == clock.availability() && restored.value() == clock.value(),
                                                                        "codec preserves time quality and value");
                                                      }
                                                      witness::Encoder encoder;
                                                      encoder.time(RawTime::doubtful(civil));
                                                      bool refused = false;
                                                      try {
                                                          witness::Decoder legacy(encoder.bytes, false);
                                                          (void)legacy.time();
                                                      } catch (const std::invalid_argument&) {
                                                          refused = true;
                                                      }
                                                      checks.expect(refused, "legacy format cannot silently acquire doubtful-time meaning");
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};
}  // namespace
