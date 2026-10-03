/** @brief ADR-004 Decision 9: segment layout, the persisting sink, durable confirmation, power-cut outcomes and storage health (issue #91). */

import std;
import mddlog.adapter.auditstore;
import mddlog.core.auditring;
import mddlog.sinks.auditsink;
import mddlog.adapter.auditdrain;
import speclab;

namespace {

// NOLINTBEGIN(hicpp-signed-bitwise): building and altering stored test bytes.

using namespace mddlog::adapter;
using mddlog::core::AuditEvent;
using Operation   = InMemoryStorageMedium::Operation;
using Effect      = InMemoryStorageMedium::Effect;
using Unconfirmed = InMemoryStorageMedium::Unconfirmed;

constexpr std::string_view streamName = "device-42/boot-7";

[[nodiscard]] AuditEvent makeEvent(std::string_view stream, std::uint64_t sequence, std::size_t detailSize = 12) {
    AuditEvent  event;
    std::string detail = "step " + std::to_string(sequence);
    detail.resize(std::max(detail.size(), detailSize), 'x');
    (void)event.assign({.action = "therapy.rate.set", .target = "pump/channel-A", .detail = detail}, stream, sequence);
    return event;
}

/** @brief Records of the given detail size that fit in one 2048-byte segment of the test configuration. */
[[nodiscard]] std::uint64_t perSegment(std::size_t detailSize) {
    const auto encoded = CanonicalRecord::encode(makeEvent(streamName, 1, detailSize));
    if (!encoded)
        return 0;
    const auto frame   = recordFrameSize(encoded->size());
    const auto opening = encodeSegmentOpening(streamName, 0, 1).size();
    return (2048 - opening) / frame;
}

/** @brief A sink over an in-memory medium, with a recorded clock and loss reports. Heap-allocated: the sink's callbacks point into it. */
struct Rig {
    explicit Rig(std::size_t capacity, bool eligible = true) : medium(capacity, eligible) {}

    [[nodiscard]] static StorageConfig config(std::size_t recordBound = 1) {
        StorageConfig out;
        out.segmentSize        = 2048;
        out.segmentCount       = 12;
        out.maxProducerStreams = 1;
        out.sync.recordBound   = recordBound;
        return out;
    }
    void start(StorageConfig declared) {
        declared.clock = [this] {
            return now;
        };
        declared.reportLoss = [this](std::uint64_t count) {
            losses.push_back(count);
        };
        auto made = PersistingAuditSink::create(medium, std::move(declared));
        sink      = *made;
    }
    void start(std::size_t recordBound = 1) {
        start(config(recordBound));
    }

    InMemoryStorageMedium                 medium;
    std::shared_ptr<PersistingAuditSink>  sink;
    std::chrono::steady_clock::time_point now;
    std::vector<std::uint64_t>            losses;
};

[[nodiscard]] std::unique_ptr<Rig> makeRig(std::size_t capacity = 12, bool eligible = true) {
    return std::make_unique<Rig>(capacity, eligible);
}

/** @brief Stored records of a stream, checked as a chain from H_0: the count that checked, and whether every stored record did. */
struct ChainCheck {
    std::size_t  stored   = 0;
    bool         allOk    = true;
    std::size_t  trailing = 0;
    Sha256Digest head{};
};

[[nodiscard]] ChainCheck checkStored(StorageMedium& medium, std::string_view stream) {
    const auto         read = readStoredStream(medium, stream);
    AuditChainVerifier verifier{stream};
    ChainCheck         out;
    out.stored   = read.records().size();
    out.trailing = read.trailingBytesOfLastSegment();
    for (const auto& record : read.records())
        out.allOk = out.allOk && verifier.check(record.bytes, record.digest) == ChainFinding::Ok;
    out.head = verifier.headDigest();
    return out;
}

/** @brief The digest H_k of a fresh chain over events 1..k with the default detail, to compare a claim against. */
[[nodiscard]] Sha256Digest chainDigestAt(std::uint64_t position, std::size_t detailSize = 12) {
    AuditChain   chain{streamName};
    Sha256Digest head = chainInitialValue;
    for (std::uint64_t k = 1; k <= position; ++k) {
        const auto chained = chain.append(makeEvent(streamName, k, detailSize));
        head               = chained->digest;
    }
    return head;
}

const speclab::Register layoutBytes{
    "Stored bytes follow layout version 1: CRC-32C, preamble, header and record frames",
    "unit",
    [] {
        return speclab::Test("audit-storage-layout")
            .Then("CRC-32C has the published check value and chains across pieces",
                  [] {
                      speclab::core::Checks     checks;
                      const std::string_view    text  = "123456789";
                      const auto                bytes = std::as_bytes(std::span{text});
                      std::vector<std::uint8_t> raw;
                      for (const auto byte : bytes)
                          raw.push_back(static_cast<std::uint8_t>(byte));
                      checks.expect(crc32c(raw) == 0xE3069283U, "check value of 123456789");
                      const auto head = crc32c(std::span{raw}.first(4));
                      checks.expect(crc32c(std::span{raw}.subspan(4), head) == 0xE3069283U, "continuing from a previous value");
                      checks.raise();
                  })
            .Then("a segment opening is the preamble and a header frame, as the ADR's vector",
                  [] {
                      speclab::core::Checks              checks;
                      const auto                         opening = encodeSegmentOpening(streamName, 0, 1);
                      const std::array<std::uint8_t, 45> expected{0x6d, 0x64, 0x6c, 0x67, 0x00, 0x01,                          // preamble
                                                                  0x01, 0x00, 0x00, 0x00, 0x1e,                                // header frame: type, length 30
                                                                  0x00, 0x00, 0x00, 0x00,                                      // segmentIndex 0
                                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,              // firstSequence 1
                                                                  0x00, 0x10, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x2d, 0x34,  // "device-4
                                                                  0x32, 0x2f, 0x62, 0x6f, 0x6f, 0x74, 0x2d, 0x37,              // 2/boot-7"
                                                                  0x78, 0x0d, 0x8e, 0x4f};                                     // check
                      checks.expect(std::ranges::equal(opening, expected), "the first 45 bytes of the vector's first segment");
                      checks.expect(encodeSegmentOpening("not a valid id", 0, 1).empty(), "no opening for an identity AuditEvent rejects");
                      checks.expect(maxSegmentOpeningSize == 125 && maxRecordFrameSize == 813, "the sizes 9.6 counts the reserve in");
                      checks.raise();
                  })
            .Then("a segment scans back to its header and records, and nothing else",
                  [] {
                      speclab::core::Checks     checks;
                      AuditChain                chain{streamName};
                      auto                      segment = encodeSegmentOpening(streamName, 3, 7);
                      std::vector<Sha256Digest> digests;
                      for (std::uint64_t k = 7; k < 10; ++k) {
                          const auto chained = chain.append(makeEvent(streamName, k - 6));
                          const auto frame   = encodeRecordFrame(chained->canonical.bytes(), chained->digest);
                          segment.insert(segment.end(), frame.begin(), frame.end());
                          digests.push_back(chained->digest);
                      }
                      const auto scan = scanSegment(segment);
                      checks.expect(scan.status == SegmentStatus::Readable && scan.layoutVersion == 1, "readable, layout 1");
                      checks.expect(scan.header && scan.header->segmentIndex == 3 && scan.header->firstSequence == 7 && scan.header->streamId == streamName,
                                    "header fields");
                      checks.expect(scan.records.size() == 3 && scan.records[1].digest == digests[1], "three records with their digests");
                      checks.expect(scan.validEnd == segment.size() && scan.trailingBytes == 0, "no trailing bytes");
                      checks.raise();
                  })
            .Then("a cut inside a record frame leaves the header, no record and the trailing bytes, at every offset",
                  [] {
                      speclab::core::Checks checks;
                      AuditChain            chain{streamName};
                      const auto            opening         = encodeSegmentOpening(streamName, 0, 1);
                      const auto            chained         = chain.append(makeEvent(streamName, 1));
                      const auto            frame           = encodeRecordFrame(chained->canonical.bytes(), chained->digest);
                      bool                  stopsEverywhere = true;
                      for (std::size_t kept = 0; kept < frame.size(); ++kept) {
                          auto cut = opening;
                          cut.insert(cut.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(kept));
                          const auto scan = scanSegment(cut);
                          stopsEverywhere = stopsEverywhere && scan.status == SegmentStatus::Readable && scan.records.empty() && scan.trailingBytes == kept
                                            && scan.validEnd == opening.size();
                      }
                      checks.expect(stopsEverywhere, "every proper prefix of the frame is trailing bytes, not a record");
                      auto whole = opening;
                      whole.insert(whole.end(), frame.begin(), frame.end());
                      checks.expect(scanSegment(whole).records.size() == 1, "the whole frame is a record");
                      checks.raise();
                  })
            .Then("changing any byte of a record frame other than its length removes the record",
                  [] {
                      speclab::core::Checks checks;
                      AuditChain            chain{streamName};
                      const auto            opening    = encodeSegmentOpening(streamName, 0, 1);
                      const auto            chained    = chain.append(makeEvent(streamName, 1));
                      const auto            frame      = encodeRecordFrame(chained->canonical.bytes(), chained->digest);
                      bool                  allRefused = true;
                      for (std::size_t at = 0; at < frame.size(); ++at) {
                          if (at >= 1 && at <= 4)  // the length: a different length is a different frame, covered by the cut test
                              continue;
                          auto bytes = opening;
                          bytes.insert(bytes.end(), frame.begin(), frame.end());
                          bytes[opening.size() + at] ^= 0x01;
                          allRefused                  = allRefused && scanSegment(bytes).records.empty();
                      }
                      checks.expect(allRefused, "type, payload and check each fail rule 2 or rule 5");
                      checks.raise();
                  })
            .Then("erased and zeroed space never reads as a frame",
                  [] {
                      speclab::core::Checks checks;
                      for (const std::uint8_t fill : {std::uint8_t{0x00}, std::uint8_t{0xFF}}) {
                          auto bytes = encodeSegmentOpening(streamName, 0, 1);
                          bytes.insert(bytes.end(), 600, fill);
                          const auto scan = scanSegment(bytes);
                          checks.expect(scan.records.empty() && scan.trailingBytes == 600, "unused space is trailing bytes");
                      }
                      checks.raise();
                  })
            .Then("a segment without a valid preamble or header, or in an unknown layout, reads nothing",
                  [] {
                      speclab::core::Checks checks;
                      auto                  opening  = encodeSegmentOpening(streamName, 0, 1);
                      auto                  version2 = opening;
                      version2[5]                    = 2;
                      auto version0                  = opening;
                      version0[5]                    = 0;
                      auto badMagic                  = opening;
                      badMagic[0]                    = 0x00;
                      const std::vector<std::uint8_t> preambleOnly{opening.begin(), opening.begin() + 6};
                      auto                            badHeader = opening;
                      badHeader[20]                            ^= 0x01;
                      checks.expect(scanSegment(version2).status == SegmentStatus::UnknownLayoutVersion && scanSegment(version2).layoutVersion == 2,
                                    "an unknown version is named, never guessed at");
                      checks.expect(scanSegment(version0).status == SegmentStatus::NoValidPreamble, "version 0 is never valid");
                      checks.expect(scanSegment(badMagic).status == SegmentStatus::NoValidPreamble, "wrong magic");
                      checks.expect(scanSegment({}).status == SegmentStatus::NoValidPreamble, "empty");
                      checks.expect(scanSegment(preambleOnly).status == SegmentStatus::NoValidHeader, "preamble and no header");
                      checks.expect(scanSegment(badHeader).status == SegmentStatus::NoValidHeader && scanSegment(badHeader).records.empty(),
                                    "a header whose check fails");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register configuration{
    "The declared configuration is checked against the reserve before anything is written",
    "unit",
    [] {
        return speclab::Test("audit-storage-configuration")
            .Then("the reserve is the segments that hold N + S + 3 frames of 813 bytes, plus one",
                  [] {
                      speclab::core::Checks checks;
                      // 4096-byte segments hold floor((4096 - 125) / 813) = 4 frames: N=16, S=2 gives 21 frames, 6 segments, plus one.
                      checks.expect(ledgerReserveSegments(4096, 16, 2) == 7, "7 segments");
                      checks.expect(ledgerReserveSegments(2048, 12, 1) == 9, "2 frames per segment: 16 frames, 8 segments, plus one");
                      checks.expect(ledgerReserveSegments(900, 12, 1) == 0, "a segment that cannot hold one frame has no reserve");
                      checks.raise();
                  })
            .Then("a medium that cannot hold the reserve and one more segment is refused",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig      = makeRig();
                      auto                  declared = rig->config();
                      declared.segmentCount          = 7;  // 2 frames per segment: N + S + 3 = 11 frames, 6 segments, plus one is the reserve alone
                      const auto refused             = PersistingAuditSink::create(rig->medium, declared);
                      checks.expect(!refused && refused.error() == StorageConfigError::MediumTooSmall, "N equal to the reserve is refused");
                      declared.segmentCount = 8;
                      checks.expect(PersistingAuditSink::create(rig->medium, declared).has_value(), "the reserve and one segment is accepted");
                      checks.raise();
                  })
            .Then("a segment too small for its opening and the largest frame, no streams and an invalid policy are refused",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig      = makeRig();
                      auto                  declared = rig->config();
                      declared.segmentSize           = 937;
                      checks.expect(PersistingAuditSink::create(rig->medium, declared).error() == StorageConfigError::SegmentTooSmall, "937 < 125 + 813");
                      declared                    = rig->config();
                      declared.maxProducerStreams = 0;
                      checks.expect(PersistingAuditSink::create(rig->medium, declared).error() == StorageConfigError::NoProducerStreams, "S of 0");
                      declared                  = rig->config();
                      declared.sync.recordBound = 0;
                      checks.expect(PersistingAuditSink::create(rig->medium, declared).error() == StorageConfigError::InvalidSyncPolicy, "record bound 0");
                      declared               = rig->config();
                      declared.sync.ageBound = std::chrono::nanoseconds::zero();
                      checks.expect(PersistingAuditSink::create(rig->medium, declared).error() == StorageConfigError::InvalidSyncPolicy, "age bound 0");
                      checks.expect(rig->medium.calls(Operation::Open) == 0 && rig->medium.calls(Operation::Append) == 0, "nothing was written");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register durableConfirmation{
    "A record is durably confirmed only when a sync answered durable, and the position is published as a prefix",
    "integration",
    [] {
        return speclab::Test("audit-storage-durable")
            .Then("accept is a hand-off: nothing is durable before a sync, and the position rises with each confirmation",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(3);
                      checks.expect(rig->sink->accept(makeEvent(streamName, 1)) && rig->sink->accept(makeEvent(streamName, 2)), "two events handed off");
                      checks.expect(rig->sink->durablePosition(streamName) == 0 && !rig->sink->durableClaim(streamName), "handed off, not durable, no claim");
                      checks.expect(rig->medium.calls(Operation::Sync) == 0, "no sync was issued under the record bound");
                      checks.expect(rig->sink->accept(makeEvent(streamName, 3)), "the third event reaches the record bound");
                      checks.expect(rig->sink->durablePosition(streamName) == 3, "durable through 3 once the sync answered durable");
                      const auto claim = rig->sink->durableClaim(streamName);
                      checks.expect(claim && claim->position == 3 && claim->digest == chainDigestAt(3) && claim->streamId == streamName,
                                    "the claim carries H_3 and nothing above the confirmed position");
                      (void)rig->sink->accept(makeEvent(streamName, 4));
                      checks.expect(rig->sink->durablePosition(streamName) == 3, "an appended record is not confirmed by the next append");
                      checks.raise();
                  })
            .Then("flush and close sync the open segments, and a closed sink accepts nothing more",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(100);
                      for (std::uint64_t k = 1; k <= 4; ++k)
                          (void)rig->sink->accept(makeEvent(streamName, k));
                      checks.expect(rig->sink->durablePosition(streamName) == 0, "under the bound");
                      rig->sink->flush();
                      checks.expect(rig->sink->durablePosition(streamName) == 4, "flush confirms 4");
                      (void)rig->sink->accept(makeEvent(streamName, 5));
                      rig->sink->close();
                      checks.expect(rig->sink->durablePosition(streamName) == 5, "close syncs once more");
                      checks.expect(!rig->sink->accept(makeEvent(streamName, 6)), "closed");
                      checks.raise();
                  })
            .Then("the age bound confirms an idle sink on tick, by the declared clock",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig      = makeRig();
                      auto                  declared = rig->config(100);
                      declared.sync.ageBound         = std::chrono::seconds{10};
                      rig->start(declared);
                      (void)rig->sink->accept(makeEvent(streamName, 1));
                      rig->now += std::chrono::seconds{5};
                      rig->sink->tick();
                      checks.expect(rig->sink->durablePosition(streamName) == 0, "5 s is under the bound");
                      rig->now += std::chrono::seconds{5};
                      rig->sink->tick();
                      checks.expect(rig->sink->durablePosition(streamName) == 1, "10 s reaches it");
                      checks.raise();
                  })
            .Then("a medium that is not eligible stores records and never confirms one",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig(12, false);
                      rig->start(1);
                      for (std::uint64_t k = 1; k <= 3; ++k)
                          checks.expect(rig->sink->accept(makeEvent(streamName, k)), "stored");
                      const auto health = rig->sink->health();
                      checks.expect(health.streams.size() == 1 && health.streams[0].state == StreamStorageState::Persisting, "unsupported is not a failure");
                      checks.expect(health.streams[0].durablePosition == 0 && health.streams[0].appendedPosition == 3, "appended 3, durable 0");
                      checks.expect(!rig->sink->durableClaim(streamName), "no anchor claim ever covers it");
                      checks.expect(health.counters.syncUnsupported == 1 && health.counters.syncFailures == 0, "counted once, not as a failure");
                      checks.expect(rig->medium.calls(Operation::Sync) == 1, "the answer is every time, so it is not asked again");
                      rig->sink->flush();
                      checks.expect(rig->sink->durablePosition(streamName) == 0, "flush changes nothing");
                      const auto stored = checkStored(rig->medium, streamName);
                      checks.expect(stored.stored == 3 && stored.allOk, "the records are stored and chain");
                      checks.raise();
                  })
            .Then("stored records verify, and the durable claim anchors them through the confirmed position",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(1);
                      mddlog::core::AuditRing<8> ring{streamName};
                      AuditSinkAdapter           adapter;
                      checks.expect(adapter.addRing(ring) == AuditRingRegistration::Registered, "ring registered");
                      adapter.setSink(rig->sink);
                      for (int k = 0; k < 4; ++k)
                          checks.expect(ring.tryRecord({.action = "therapy.rate.set", .target = "pump/channel-A", .detail = "drained"}).wasAdmitted(),
                                        "admitted");
                      const auto drained = adapter.drainOnce();
                      checks.expect(drained.handedOff == 4 && drained.status == AuditDrainStatus::Completed, "handed off through the existing audit path");
                      InMemoryAnchorProvider provider{"witness-1"};
                      RetainedPosition       retained;
                      AnchorVerifier         verifier{provider, retained};
                      const auto             claim = rig->sink->durableClaim(streamName);
                      checks.expect(claim && claim->position == 4, "durable through 4");
                      checks.expect(std::holds_alternative<AnchorStamp>(provider.advance(*claim)), "the provider accepts the claim");
                      const auto stored = readStoredStream(rig->medium, streamName);
                      const auto report = verifier.verify(streamName, stored.records());
                      checks.expect(report.verdict == Verdict::Anchored && report.anchoredThrough == std::uint64_t{4}, "Anchored through the durable position");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register rotation{"A stream spans numbered segments, synced before each rotation, and a record is never split", "integration", [] {
                                     return speclab::Test("audit-storage-segments")
                                         .Then("records continue across segments with no gap and the header names the first sequence",
                                               [] {
                                                   speclab::core::Checks checks;
                                                   auto                  rig = makeRig();
                                                   rig->start(1000);  // no policy sync: only the rotation syncs
                                                   const std::uint64_t total    = (2 * perSegment(160)) + 1;
                                                   std::uint64_t       accepted = 0;
                                                   for (std::uint64_t k = 1; k <= total; ++k)
                                                       accepted += rig->sink->accept(makeEvent(streamName, k, 160)) ? 1U : 0U;
                                                   checks.expect(accepted == total, "all handed off");
                                                   const auto listing = *rig->medium.segments();
                                                   checks.expect(listing.size() == 3, "three segments");
                                                   std::uint64_t expectedFirst = 1;
                                                   std::uint32_t expectedIndex = 0;
                                                   bool          headers       = true;
                                                   bool          fits          = true;
                                                   for (const auto& info : listing) {
                                                       const auto bytes = rig->medium.bytesOf(info.segment);
                                                       const auto scan  = scanSegment(bytes);
                                                       headers          = headers && scan.header && scan.header->segmentIndex == expectedIndex
                                                                 && scan.header->firstSequence == expectedFirst && scan.trailingBytes == 0;
                                                       fits           = fits && bytes.size() <= 2048;
                                                       expectedFirst += scan.records.size();
                                                       ++expectedIndex;
                                                   }
                                                   checks.expect(headers, "indices 0.., firstSequence follows the last record, no trailing bytes");
                                                   checks.expect(fits, "no segment exceeds its declared size");
                                                   const auto stored = checkStored(rig->medium, streamName);
                                                   checks.expect(stored.stored == total && stored.allOk, "one chain across the segments");
                                                   checks.expect(rig->medium.confirmedOf(listing.front().segment)
                                                                     == rig->medium.bytesOf(listing.front().segment).size(),
                                                                 "the first segment was confirmed up to its end before the next opened");
                                                   checks.raise();
                                               })
                                         .Then("an instance is not continued: a stream identity the medium already holds is refused",
                                               [] {
                                                   speclab::core::Checks checks;
                                                   auto                  rig = makeRig();
                                                   rig->start(1);
                                                   (void)rig->sink->accept(makeEvent(streamName, 1));
                                                   rig->sink->close();
                                                   rig->medium.restart(Unconfirmed::Dropped);
                                                   auto again = PersistingAuditSink::create(rig->medium, rig->config(1));
                                                   checks.expect(again.has_value()
                                                                     && std::ranges::find((*again)->recovery().streamsHeld, std::string{streamName})
                                                                            != (*again)->recovery().streamsHeld.end(),
                                                                 "the startup check lists the instance the medium holds");
                                                   checks.expect(!(*again)->accept(makeEvent(streamName, 2)),
                                                                 "a restart starts a new instance, never continues the old one");
                                                   checks.expect((*again)->health().counters.streamIdentityInUse == 1, "counted");
                                                   checks.expect((*again)->accept(makeEvent("device-42/boot-8", 1)), "a new identity is accepted");
                                                   checks.expect(checkStored(rig->medium, streamName).stored == 1, "the old instance is untouched");
                                                   checks.raise();
                                               })
                                         .Then("a stream beyond the declared maximum is refused",
                                               [] {
                                                   speclab::core::Checks checks;
                                                   auto                  rig = makeRig();
                                                   rig->start(1);
                                                   checks.expect(rig->sink->accept(makeEvent(streamName, 1)), "first stream");
                                                   checks.expect(!rig->sink->accept(makeEvent("device-42/other", 1)), "second stream, S = 1");
                                                   checks.expect(rig->sink->health().counters.streamLimitRefused == 1, "counted");
                                                   checks.raise();
                                               })
                                         .Execute();
                                 }};

const speclab::Register fullState{
    "A stream that needs a segment when only the reserve is free is full, not failed, and continues without a gap",
    "integration",
    [] {
        return speclab::Test("audit-storage-full")
            .Then("it appends nothing, refuses from accept, and resumes in the same chain once a segment is reclaimed",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig      = makeRig();
                      auto                  declared = rig->config(1);
                      declared.maxProducerStreams    = 2;
                      declared.segmentCount          = 12;  // reserve for S=2: ceil((12+2+3)/2)+1 = 10, so two segments are free for producers
                      rig->start(declared);
                      std::uint64_t first = 1;
                      // Stream A opens one segment; stream B takes the second free one.
                      checks.expect(rig->sink->accept(makeEvent(streamName, first++, 160)), "A opens its first segment");
                      checks.expect(rig->sink->accept(makeEvent("device-42/other", 1, 160)), "B opens the second free segment");
                      // Fill A's segment, then ask for a third.
                      std::uint64_t refusedAt = 0;
                      for (std::uint64_t k = first; k < 40; ++k) {
                          if (!rig->sink->accept(makeEvent(streamName, k, 160))) {
                              refusedAt = k;
                              break;
                          }
                      }
                      checks.expect(refusedAt != 0, "A is refused once its segment is full and only the reserve remains");
                      auto       health  = rig->sink->health();
                      const auto stateOf = [&](std::string_view name) {
                          for (const auto& stream : health.streams)
                              if (stream.streamId == name)
                                  return stream.state;
                          return StreamStorageState::Failed;
                      };
                      checks.expect(stateOf(streamName) == StreamStorageState::Full && stateOf("device-42/other") == StreamStorageState::Persisting,
                                    "A is full, B persists");
                      checks.expect(health.counters.fullEntries == 1 && health.lastIssue == StorageIssue::Full && health.freeSegmentsBeyondReserve == 0,
                                    "full is observable and counted once");
                      const auto sizeBefore = checkStored(rig->medium, streamName);
                      checks.expect(!rig->sink->accept(makeEvent(streamName, refusedAt, 160)), "a retry is refused again");
                      checks.expect(checkStored(rig->medium, streamName).stored == sizeBefore.stored && checkStored(rig->medium, streamName).trailing == 0,
                                    "no partial frame was written");
                      checks.expect(rig->sink->health().counters.fullEntries == 1, "the full state is entered once, not per retry");
                      // Retention would reclaim B's segment (Decision 10); the test plays that role.
                      const auto listing  = *rig->medium.segments();
                      SegmentRef bSegment = 0;
                      for (const auto& info : listing)
                          if (scanSegment(rig->medium.bytesOf(info.segment)).header->streamId == "device-42/other")
                              bSegment = info.segment;
                      checks.expect(rig->medium.reclaim(bSegment), "a segment was freed");
                      checks.expect(rig->sink->accept(makeEvent(streamName, refusedAt, 160)), "A resumes with the event that was refused");
                      health = rig->sink->health();
                      checks.expect(stateOf(streamName) == StreamStorageState::Persisting, "and leaves the full state");
                      const auto after = checkStored(rig->medium, streamName);
                      checks.expect(after.stored == refusedAt && after.allOk, "its chain continues without a gap");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register failures{
    "A storage failure ends durability for the instance and is observable without going through the failing path",
    "integration",
    [] {
        return speclab::Test("audit-storage-failures")
            .Then("an append failure refuses the event and the rest of the instance, and writes nothing more",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(1);
                      rig->medium.inject({.operation = Operation::Append, .ordinal = 4, .effect = Effect::Fail});
                      for (std::uint64_t k = 1; k <= 3; ++k)
                          checks.expect(rig->sink->accept(makeEvent(streamName, k)), "stored and confirmed");
                      const auto sizeBefore = rig->medium.bytesOf(1).size();
                      checks.expect(!rig->sink->accept(makeEvent(streamName, 4)), "the failing append is refused");
                      checks.expect(!rig->sink->accept(makeEvent(streamName, 4)) && !rig->sink->accept(makeEvent(streamName, 5)), "so is everything after it");
                      checks.expect(rig->medium.bytesOf(1).size() == sizeBefore && rig->medium.calls(Operation::Append) == 4,
                                    "no retry at the same offset, no new segment: nothing more was written");
                      const auto health = rig->sink->health();
                      checks.expect(health.streams[0].state == StreamStorageState::Failed && health.streams[0].cause == StorageIssue::AppendFailed
                                        && health.counters.appendFailures == 1 && health.lastIssue == StorageIssue::AppendFailed,
                                    "the failure and its cause are observable");
                      checks.expect(health.streams[0].durablePosition == 3, "the durable position stays where it was");
                      checks.expect(rig->losses.empty() && health.counters.notDurableAtFailure == 0, "everything accepted had been confirmed: no loss");
                      checks.raise();
                  })
            .Then("events accepted but not confirmed when an instance fails are counted and reported as losses after admission",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(100);
                      for (std::uint64_t k = 1; k <= 4; ++k)
                          (void)rig->sink->accept(makeEvent(streamName, k));
                      rig->medium.inject({.operation = Operation::Sync, .ordinal = 1, .effect = Effect::Fail});
                      rig->sink->flush();
                      const auto health = rig->sink->health();
                      checks.expect(health.streams[0].state == StreamStorageState::Failed && health.streams[0].cause == StorageIssue::SyncFailed
                                        && health.counters.syncFailures == 1,
                                    "a sync failure ends the instance");
                      checks.expect(health.counters.notDurableAtFailure == 4 && rig->losses == std::vector<std::uint64_t>{4},
                                    "4 accepted, 0 confirmed: 4 losses reported");
                      checks.expect(rig->sink->durablePosition(streamName) == 0 && !rig->sink->durableClaim(streamName), "nothing is claimed durable");
                      checks.raise();
                  })
            .Then("a sync that fails right after a successful append still counts that event as handed off, and not durable",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(1);
                      rig->medium.inject({.operation = Operation::Sync, .ordinal = 2, .effect = Effect::Fail});
                      checks.expect(rig->sink->accept(makeEvent(streamName, 1)), "confirmed");
                      checks.expect(rig->sink->accept(makeEvent(streamName, 2)), "handed off: it was appended before the sync failed");
                      checks.expect(!rig->sink->accept(makeEvent(streamName, 3)), "the instance has ended");
                      const auto health = rig->sink->health();
                      checks.expect(health.streams[0].durablePosition == 1 && health.streams[0].appendedPosition == 2
                                        && health.counters.notDurableAtFailure == 1 && rig->losses == std::vector<std::uint64_t>{1},
                                    "one event lost after admission, reported");
                      checks.raise();
                  })
            .Then("open failures and a noSpace answer are told apart",
                  [] {
                      speclab::core::Checks checks;
                      auto                  failed = makeRig();
                      failed->start(1);
                      failed->medium.inject({.operation = Operation::Open, .ordinal = 1, .effect = Effect::Fail});
                      checks.expect(!failed->sink->accept(makeEvent(streamName, 1)), "open failed");
                      auto health = failed->sink->health();
                      checks.expect(health.streams[0].cause == StorageIssue::OpenFailed && health.counters.openFailures == 1 && health.counters.noSpace == 0,
                                    "OpenFailed");
                      auto space = makeRig();
                      space->start(1);
                      space->medium.inject({.operation = Operation::Open, .ordinal = 1, .effect = Effect::NoSpace});
                      checks.expect(!space->sink->accept(makeEvent(streamName, 1)), "no space");
                      health = space->sink->health();
                      checks.expect(health.streams[0].cause == StorageIssue::NoSpace && health.counters.noSpace == 1,
                                    "a noSpace answer means the declared capacity was wrong: reported as such");
                      checks.raise();
                  })
            .Then("a retry of the last appended event is accepted once, other bytes under that sequence are an integrity fault",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(1);
                      checks.expect(rig->sink->accept(makeEvent(streamName, 1)) && rig->sink->accept(makeEvent(streamName, 2)), "two events");
                      const auto appends = rig->medium.calls(Operation::Append);
                      checks.expect(rig->sink->accept(makeEvent(streamName, 2)), "the same event again is accepted");
                      checks.expect(rig->medium.calls(Operation::Append) == appends && checkStored(rig->medium, streamName).stored == 2,
                                    "without a second append");
                      checks.expect(!rig->sink->accept(makeEvent(streamName, 2, 40)), "different bytes, same sequence");
                      const auto health = rig->sink->health();
                      checks.expect(health.streams[0].state == StreamStorageState::Failed && health.streams[0].cause == StorageIssue::DuplicateMismatch
                                        && health.counters.duplicateMismatch == 1,
                                    "an integrity fault ends the instance");
                      checks.raise();
                  })
            .Then("a gap, a regression and a first event that is not 1 are integrity faults, never silently stored",
                  [] {
                      speclab::core::Checks checks;
                      auto                  gap = makeRig();
                      gap->start(1);
                      (void)gap->sink->accept(makeEvent(streamName, 1));
                      checks.expect(!gap->sink->accept(makeEvent(streamName, 3)), "a gap");
                      checks.expect(gap->sink->health().streams[0].cause == StorageIssue::SequenceOutOfOrder
                                        && checkStored(gap->medium, streamName).stored == 1,
                                    "out of order, and the chain admits no gap");
                      auto back = makeRig();
                      back->start(1);
                      (void)back->sink->accept(makeEvent(streamName, 1));
                      (void)back->sink->accept(makeEvent(streamName, 2));
                      checks.expect(!back->sink->accept(makeEvent(streamName, 1)), "a regression");
                      checks.expect(back->sink->health().counters.sequenceOutOfOrder == 1, "counted");
                      auto late = makeRig();
                      late->start(1);
                      checks.expect(!late->sink->accept(makeEvent(streamName, 2)), "a first event of 2");
                      checks.expect(late->sink->health().counters.sequenceOutOfOrder == 1 && late->medium.calls(Operation::Open) == 0,
                                    "refused before anything is written");
                      checks.raise();
                  })
            .Then("a failure is never reported as an audit event: the sink emits nothing but the counters",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(1);
                      rig->medium.inject({.operation = Operation::Append, .ordinal = 2, .effect = Effect::Fail});
                      (void)rig->sink->accept(makeEvent(streamName, 1));
                      const auto appends = rig->medium.calls(Operation::Append);
                      (void)rig->sink->accept(makeEvent(streamName, 2));
                      checks.expect(rig->medium.calls(Operation::Append) == appends + 1,
                                    "the failed append is the only write attempted after the failure began");
                      checks.expect(checkStored(rig->medium, streamName).stored == 1, "no record describes the failure on the failing storage");
                      checks.raise();
                  })
            .Execute();
    }};

/** @brief What a power cut at one point left behind, seen from a reader after the restart. */
struct CutOutcome {
    std::uint64_t                    publishedDurable = 0;
    ChainCheck                       stored;
    std::size_t                      recoveryFindings  = 0;
    bool                             claimWithinStored = true;
    bool                             noTrailingFinding = true;
    std::vector<RecoveryFindingKind> kinds;
};

/**
 * @brief Write events until the sink stops accepting, with a fault injected at (operation, ordinal), then lose power under `policy` and read what remains.
 * Every outcome is also checked against the invariants that hold for every cut: what remains chains from H_0, and the durable position the host was told of
 * is never above it.
 */
[[nodiscard]] CutOutcome
cutAt(Operation operation, std::size_t ordinal, Effect effect, std::size_t partial, Unconfirmed policy, std::size_t detail = 12, std::uint64_t events = 5) {
    auto rig = makeRig();
    rig->start(1);
    rig->medium.inject({.operation = operation, .ordinal = ordinal, .effect = effect, .partialBytes = partial});
    for (std::uint64_t k = 1; k <= events; ++k) {
        if (!rig->sink->accept(makeEvent(streamName, k, detail)))
            break;
    }
    CutOutcome out;
    out.publishedDurable = rig->sink->durablePosition(streamName);
    const auto claim     = rig->sink->durableClaim(streamName);
    rig->medium.restart(policy);
    out.stored            = checkStored(rig->medium, streamName);
    out.claimWithinStored = !claim || (claim->position <= out.stored.stored && claim->digest == chainDigestAt(claim->position, detail));
    auto       restarted  = PersistingAuditSink::create(rig->medium, rig->config(1));
    const auto report     = (*restarted)->recovery();
    out.recoveryFindings  = report.findings.size();
    for (const auto& finding : report.findings)
        out.kinds.push_back(finding.kind);
    return out;
}

const std::array<Unconfirmed, 4> allPolicies{Unconfirmed::Dropped, Unconfirmed::Kept, Unconfirmed::Half, Unconfirmed::Erased};

const speclab::Register powerCuts{
    "Every power-cut point of ADR-004 9.5 leaves an outcome the reader reports exactly",
    "integration",
    [] {
        return speclab::Test("audit-storage-power-cuts")
            .Then("whatever the cut, what remains chains from H_0 and the durable position told to the host is never above it",
                  [] {
                      speclab::core::Checks checks;
                      bool                  chains  = true;
                      bool                  within  = true;
                      std::size_t           cutsRun = 0;
                      for (const auto operation : {Operation::Open, Operation::Append, Operation::Sync}) {
                          for (const auto effect : {Effect::CutBefore, Effect::CutPartial, Effect::CutAfter}) {
                              for (std::size_t ordinal = 1; ordinal <= 6; ++ordinal) {
                                  for (const std::size_t partial : {std::size_t{0}, std::size_t{7}, std::size_t{60}}) {
                                      for (const auto policy : allPolicies) {
                                          const auto out = cutAt(operation, ordinal, effect, partial, policy, 160, 12);
                                          chains         = chains && out.stored.allOk;
                                          within         = within && out.claimWithinStored && out.publishedDurable <= out.stored.stored;
                                          ++cutsRun;
                                      }
                                  }
                              }
                          }
                      }
                      checks.expect(cutsRun == std::size_t{3} * 3 * 6 * 3 * 4, "every operation, effect, ordinal, partial length and loss policy ran");
                      checks.expect(chains, "the surviving prefix always chains");
                      checks.expect(within, "no durable position or claim ever exceeded what the medium holds");
                      checks.raise();
                  })
            .Then("before the append of record 3: the stream ends at 2, nothing partial, and no record was claimed",
                  [] {
                      speclab::core::Checks checks;
                      for (const auto policy : allPolicies) {
                          const auto out = cutAt(Operation::Append, 3, Effect::CutBefore, 0, policy);
                          checks.expect(out.stored.stored == 2 && out.stored.trailing == 0 && out.publishedDurable == 2 && out.recoveryFindings == 0,
                                        "record 3 is lost, the durable position was 2");
                      }
                      checks.raise();
                  })
            .Then("during the append of record 3: absent, partial or complete, and only the confirmed prefix is guaranteed",
                  [] {
                      speclab::core::Checks checks;
                      const auto            dropped = cutAt(Operation::Append, 3, Effect::CutPartial, 20, Unconfirmed::Dropped);
                      checks.expect(dropped.stored.stored == 2 && dropped.stored.trailing == 0, "partial bytes that were never confirmed may vanish");
                      const auto kept = cutAt(Operation::Append, 3, Effect::CutPartial, 20, Unconfirmed::Kept);
                      checks.expect(kept.stored.stored == 2 && kept.stored.trailing == 20,
                                    "or stay: the reader reports 20 trailing bytes and parses no record from them");
                      checks.expect(kept.recoveryFindings == 1 && kept.kinds[0] == RecoveryFindingKind::TrailingBytes, "the startup check reports them");
                      const auto erased = cutAt(Operation::Append, 3, Effect::CutPartial, 20, Unconfirmed::Erased);
                      checks.expect(erased.stored.stored == 2 && erased.stored.trailing == 20, "erased bytes (0xFF) never read as a frame");
                      const auto complete = cutAt(Operation::Append, 3, Effect::CutAfter, 0, Unconfirmed::Kept);
                      checks.expect(complete.stored.stored == 3 && complete.publishedDurable == 2,
                                    "a complete valid frame is a record, past the durable position");
                      const auto half = cutAt(Operation::Append, 3, Effect::CutAfter, 0, Unconfirmed::Half);
                      checks.expect(half.stored.stored == 2 && half.stored.trailing > 0, "half of it is trailing bytes");
                      checks.raise();
                  })
            .Then("after the append and before the sync: an unconfirmed record may be present, and is never claimed",
                  [] {
                      speclab::core::Checks checks;
                      const auto            kept = cutAt(Operation::Sync, 3, Effect::CutBefore, 0, Unconfirmed::Kept);
                      checks.expect(kept.stored.stored == 3 && kept.publishedDurable == 2 && kept.claimWithinStored, "present but not durable");
                      const auto dropped = cutAt(Operation::Sync, 3, Effect::CutBefore, 0, Unconfirmed::Dropped);
                      checks.expect(dropped.stored.stored == 2 && dropped.publishedDurable == 2, "or absent");
                      checks.raise();
                  })
            .Then("during the sync: the part persisted is whatever the medium kept, and nothing was confirmed",
                  [] {
                      speclab::core::Checks checks;
                      const auto            out = cutAt(Operation::Sync, 3, Effect::CutPartial, 10, Unconfirmed::Dropped);
                      checks.expect(out.stored.stored == 2 && out.stored.trailing == 10 && out.publishedDurable == 2,
                                    "10 bytes persisted, an incomplete frame");
                      checks.raise();
                  })
            .Then("after the sync answered durable and before it was published: the record is present, and the host must treat it as unknown, not lost",
                  [] {
                      speclab::core::Checks checks;
                      for (const auto policy : allPolicies) {
                          const auto out = cutAt(Operation::Sync, 3, Effect::CutAfter, 0, policy);
                          checks.expect(out.stored.stored == 3 && out.stored.allOk, "confirmed in fact: present under every loss policy");
                          checks.expect(out.publishedDurable == 2, "but never told");
                      }
                      checks.raise();
                  })
            .Then("after the durable position was published: the record is present under every loss policy",
                  [] {
                      speclab::core::Checks checks;
                      for (const auto policy : allPolicies) {
                          const auto out = cutAt(Operation::Append, 4, Effect::CutBefore, 0, policy);
                          checks.expect(out.publishedDurable == 3 && out.stored.stored == 3 && out.stored.allOk && out.claimWithinStored,
                                        "records 1 to 3 survive, and the claim's digest is H_3");
                      }
                      checks.raise();
                  })
            .Then("during open of a new segment: absent, without a valid header, or empty, and the earlier records are untouched",
                  [] {
                      speclab::core::Checks checks;
                      // 160-byte details fill a 2048-byte segment after `full` records, so the next record rotates and opens segment 1 (open ordinal 2).
                      const std::uint64_t full   = perSegment(160);
                      const auto          before = cutAt(Operation::Open, 2, Effect::CutBefore, 0, Unconfirmed::Kept, 160, full + 3);
                      checks.expect(before.stored.stored == full && before.recoveryFindings == 0 && before.publishedDurable == full,
                                    "no segment: stream ends at the last record of segment 0");
                      const auto partial = cutAt(Operation::Open, 2, Effect::CutPartial, 10, Unconfirmed::Kept, 160, full + 3);
                      checks.expect(partial.stored.stored == full && partial.recoveryFindings == 1 && partial.kinds[0] == RecoveryFindingKind::NoValidHeader,
                                    "a segment without a valid header is reported, and no record is read from it");
                      const auto empty = cutAt(Operation::Open, 2, Effect::CutAfter, 0, Unconfirmed::Kept, 160, full + 3);
                      checks.expect(empty.stored.stored == full && empty.recoveryFindings == 0 && empty.publishedDurable == full,
                                    "a complete header with no record is an empty segment: normal");
                      const auto lost = cutAt(Operation::Open, 2, Effect::CutAfter, 0, Unconfirmed::Dropped, 160, full + 3);
                      checks.expect(lost.stored.stored == full && lost.recoveryFindings == 0, "a segment no sync confirmed may not survive at all");
                      checks.raise();
                  })
            .Then("after open and before the first sync covering the header: the first record is not confirmed, whatever remains",
                  [] {
                      speclab::core::Checks checks;
                      for (const auto policy : allPolicies) {
                          const std::uint64_t full = perSegment(160);
                          const auto          out  = cutAt(Operation::Sync, full + 1, Effect::CutBefore, 0, policy, 160, full + 3);
                          checks.expect(out.publishedDurable == full && out.stored.stored >= full && out.stored.allOk && out.claimWithinStored,
                                        "the next record opens segment 1; its sync did not complete");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register startupCheck{
    "The startup check reports exactly what it covers and nothing it does not",
    "integration",
    [] {
        return speclab::Test("audit-storage-startup-check")
            .Then("it reports segments without a header, an unknown layout, duplicate and missing indices, and trailing bytes in the last segment",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{20};
                      const auto            open = [&](std::string_view stream, std::uint32_t index, std::uint64_t first) {
                          const auto bytes = encodeSegmentOpening(stream, index, first);
                          return medium.open({.streamId = stream, .segmentIndex = index, .firstSequence = first, .bytes = bytes}).segment;
                      };
                      (void)open("a/one", 0, 1);
                      (void)open("a/one", 0, 5);  // a duplicate index
                      (void)open("a/one", 2, 9);  // index 1 is missing
                      const auto bytesB = encodeSegmentOpening("b/two", 0, 1);
                      const auto idB    = medium.open({.streamId = "b/two", .segmentIndex = 0, .firstSequence = 1, .bytes = bytesB}).segment;
                      const std::array<std::uint8_t, 9> junk{1, 2, 3, 4, 5, 6, 7, 8, 9};
                      (void)medium.append(idB, junk);  // trailing bytes in b/two's last segment
                      auto unknown = encodeSegmentOpening("c/three", 0, 1);
                      unknown[5]   = 9;
                      (void)medium.open({.streamId = "c/three", .segmentIndex = 0, .firstSequence = 1, .bytes = unknown});
                      const std::array<std::uint8_t, 3> torn{0x6d, 0x64, 0x6c};
                      (void)medium.open({.streamId = "d/four", .segmentIndex = 0, .firstSequence = 1, .bytes = torn});
                      const auto report = checkMediumAtStart(medium);
                      const auto has    = [&](RecoveryFindingKind kind) {
                          return std::ranges::any_of(report.findings, [&](const RecoveryFinding& finding) {
                              return finding.kind == kind;
                          });
                      };
                      checks.expect(report.mediumReadable && report.segmentsExamined == 6, "six segments examined");
                      checks.expect(has(RecoveryFindingKind::DuplicateSegmentIndex), "duplicate index");
                      checks.expect(has(RecoveryFindingKind::SegmentIndexGap), "a gap");
                      checks.expect(has(RecoveryFindingKind::UnknownLayoutVersion), "unknown layout");
                      checks.expect(has(RecoveryFindingKind::NoValidPreamble), "a torn preamble");
                      const auto trailing = std::ranges::find_if(report.findings, [](const RecoveryFinding& f) {
                          return f.kind == RecoveryFindingKind::TrailingBytes;
                      });
                      checks.expect(trailing != report.findings.end() && trailing->streamId == "b/two" && trailing->offset == bytesB.size()
                                        && trailing->length == 9,
                                    "trailing bytes, with their offset and length");
                      checks.expect(report.streamsHeld == std::vector<std::string>{"a/one", "b/two"}, "the instances the medium holds");
                      checks.raise();
                  })
            .Then("it reads no record frame of an earlier segment: those findings belong to the verifier",
                  [] {
                      speclab::core::Checks checks;
                      auto                  rig = makeRig();
                      rig->start(1000);
                      const std::uint64_t total = (2 * perSegment(160)) + 1;
                      for (std::uint64_t k = 1; k <= total; ++k)
                          (void)rig->sink->accept(makeEvent(streamName, k, 160));
                      rig->sink->flush();
                      const auto listing = *rig->medium.segments();
                      // Damage a record in the first segment: past its opening, inside the first record frame.
                      rig->medium.flipByte(listing.front().segment, 60);
                      const auto report = checkMediumAtStart(rig->medium);
                      checks.expect(report.findings.empty(), "the startup check finds nothing");
                      const auto stored = checkStored(rig->medium, streamName);
                      checks.expect(stored.stored < total || !stored.allOk, "while a reader of the records does see the damage");
                      checks.raise();
                  })
            .Then("a medium that cannot list its segments is reported, not read as empty",
                  [] {
                      speclab::core::Checks checks;
                      InMemoryStorageMedium medium{4};
                      medium.inject({.operation = Operation::Open, .ordinal = 1, .effect = Effect::CutBefore});
                      (void)medium.open({.streamId = "a/one", .segmentIndex = 0, .firstSequence = 1, .bytes = encodeSegmentOpening("a/one", 0, 1)});
                      const auto report = checkMediumAtStart(medium);
                      checks.expect(!report.mediumReadable && report.findings.empty(), "unreadable");
                      checks.raise();
                  })
            .Execute();
    }};

// NOLINTEND(hicpp-signed-bitwise)

}  // namespace
