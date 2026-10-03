/** @brief ADR-004 Decision 8 reference vectors, canonical serialization and per-stream chaining. */

import std;
import mddlog.adapter.auditchain;
import mddlog.core.auditevent;
import speclab;

namespace {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,hicpp-signed-bitwise): test vectors are indexed by loop position over fixed arrays; hex
// decoding shifts int nibbles.

using namespace mddlog::adapter;
using mddlog::core::AuditCategory;
using mddlog::core::AuditEvent;
using mddlog::core::AuditPhase;
using mddlog::core::RawTime;

constexpr std::string_view streamName = "device-42/boot-7";

struct Vector {
    std::string_view canonicalHex;
    std::string_view digestHex;
};

// Extracted from the byte blocks of ADR-004 8.7 (V1 to V4), not retyped.
constexpr std::array<Vector, 4> vectors{
    {
     {.canonicalHex = "000100106465766963652d34322f626f6f742d37000000000000000102030118867251f555cd15001074686572617079"
                         "2e726174652e73657400116f70657261746f723a6e757273652d3037000e70756d702f6368616e6e656c2d41000b5245"
                         "512d414c4d2d303132000552432d313700076f702d303030310100000000000000290018726174652031322e35206d4c"
                         "2f6820636f6e6669726d656400",
         .digestHex    = "5384ec4133d6baab7790b48a0fa0c8eb3d249e37d9487a886aa47b09803b9055"},
     {.canonicalHex = "000100106465766963652d34322f626f6f742d370000000000000002010100000c6465766963652e7374617274000000"
                         "096465766963652d343200000000000000000000",
         .digestHex    = "dd894a8130712adfc820f13daf1bc72f68ba701d7bf4f6cbd2b8ff31f5591fd8"},
     {.canonicalHex = "000100106465766963652d34322f626f6f742d37000000000000000304040100000000000000000009616c61726d2e61"
                         "636b0000000f616c61726d2f6f63636c7573696f6e0000000552432d30330000010000000000000000009f6161616161"
                         "616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161"
                         "616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161"
                         "616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161"
                         "6161616161616161616101",
         .digestHex    = "0a870f567b1cb9781ebde3d8bdc9d388d463ee6c38fe660256cf36e9da147027"},
     {.canonicalHex = "000100106465766963652d34322f626f6f742d370000000000000004030201ffffffffffffffff000d73657373696f6e"
                         "2e6c6f67696e000b7376633a75706461746572000973657373696f6e2f3300000000000001ffffffffffffffff0002c3"
                         "a900",
         .digestHex    = "12d523bdf4082156aecfb31c96af83a80054383470b9b06f7e6b5be447c42b07"},
     }
};

[[nodiscard]] constexpr std::uint8_t nibble(char c) noexcept {
    return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
}

[[nodiscard]] std::vector<std::uint8_t> fromHex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    return out;
}

[[nodiscard]] Sha256Digest digestFromHex(std::string_view hex) {
    Sha256Digest out{};
    const auto   raw = fromHex(hex);
    std::ranges::copy(raw, out.begin());
    return out;
}

[[nodiscard]] std::string hexOf(const Sha256Digest& d) {
    const auto text = digestToHex(d);
    return std::string{text.begin(), text.end()};
}

[[nodiscard]] AuditEvent eventFor(std::size_t vector) {
    AuditEvent event;
    switch (vector) {
        case 1:
            (void)event.assign({.category = AuditCategory::Configuration,
                                .phase    = AuditPhase::Executed,
                                .time     = RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{1767225600123456789}}),
                                .action   = "therapy.rate.set",
                                .actor    = "operator:nurse-07",
                                .target   = "pump/channel-A",
                                .requirementRef = "REQ-ALM-012",
                                .riskRef        = "RC-17",
                                .correlationId  = "op-0001",
                                .sourceSequence = 41,
                                .detail         = "rate 12.5 mL/h confirmed"},
                               streamName,
                               1);
            break;
        case 2:
            (void)event.assign({.category = AuditCategory::Lifecycle, .phase = AuditPhase::Requested, .action = "device.start", .target = "device-42"},
                               streamName,
                               2);
            break;
        case 3: {
            // 159 bytes 'a' then U+00E9: 161 bytes offered, admission cuts before the two-byte sequence.
            std::string detail(159, 'a');
            detail += "\xC3\xA9";
            (void)event.assign({.category       = AuditCategory::RiskControl,
                                .phase          = AuditPhase::Failed,
                                .time           = RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{}),
                                .action         = "alarm.ack",
                                .target         = "alarm/occlusion",
                                .riskRef        = "RC-03",
                                .sourceSequence = 0,
                                .detail         = detail},
                               streamName,
                               3);
            break;
        }
        default:
            (void)event.assign({.category       = AuditCategory::Access,
                                .phase          = AuditPhase::Confirmed,
                                .time           = RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{-1}}),
                                .action         = "session.login",
                                .actor          = "svc:updater",
                                .target         = "session/3",
                                .sourceSequence = std::numeric_limits<std::uint64_t>::max(),
                                .detail         = "\xC3\xA9"},
                               streamName,
                               4);
            break;
    }
    return event;
}

// FIPS 180-4 known answer, evaluated at compile time so the digest cannot depend on a toolchain's
// runtime: "abc".
constexpr std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
constexpr auto                        abcHex = digestToHex(sha256(abc));
static_assert(std::string_view{abcHex.data(), abcHex.size()} == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of 'abc'");

const speclab::Register canonicalVectors{
    "Canonical bytes and digests match the ADR-004 reference vectors",
    "unit",
    [] {
        return speclab::Test("audit-canonical-vectors")
            .Then("each vector's bytes and chained digest are those of Decision 8.7",
                  [] {
                      speclab::core::Checks checks;
                      Sha256Digest          previous = chainInitialValue;
                      for (std::size_t k = 1; k <= vectors.size(); ++k) {
                          const AuditEvent event   = eventFor(k);
                          const auto       encoded = encodeCanonical(event);
                          checks.expect(encoded.has_value(), "the event is encodable");
                          if (!encoded)
                              continue;
                          const auto expectedBytes = fromHex(vectors[k - 1].canonicalHex);
                          checks.expect(std::ranges::equal(encoded->bytes(), expectedBytes), "canonical bytes of V" + std::to_string(k));
                          const Sha256Digest digest = chainDigest(encoded->bytes(), previous);
                          checks.expect(hexOf(digest) == vectors[k - 1].digestHex, "digest of V" + std::to_string(k));
                          previous = digest;
                      }
                      checks.expect(eventFor(3).detailTruncated() && eventFor(3).detail().size() == 159, "V3 detail was cut at admission");
                      checks.expect(!eventFor(4).detailTruncated(), "V4 detail is kept whole");
                      checks.raise();
                  })
            .Then("sizes and the largest possible record agree with the contract",
                  [] {
                      speclab::core::Checks checks;
                      checks.expect(encodeCanonical(eventFor(1))->size() == 157 && encodeCanonical(eventFor(2))->size() == 68
                                        && encodeCanonical(eventFor(3))->size() == 251 && encodeCanonical(eventFor(4))->size() == 98,
                                    "sizes 157, 68, 251 and 98");
                      checks.expect(canonicalMaxSize == 2 + 98 + 8 + 2 + 9 + 66 + 66 + 98 + 66 + 66 + 119 + 9 + 162 + 1, "maximum record size");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register chainAndVerifier{
    "A stream instance chains canonical records and a reader finds every break",
    "unit",
    [] {
        return speclab::Test("audit-chain-verifier")
            .Then("the writer reproduces the vector digests and head",
                  [] {
                      speclab::core::Checks checks;
                      AuditChain            chain{streamName};
                      checks.expect(chain.valid() && chain.chainedThrough() == 0 && chain.headDigest() == chainInitialValue, "starts at H_0");
                      for (std::size_t k = 1; k <= vectors.size(); ++k) {
                          const auto chained = chain.append(eventFor(k));
                          checks.expect(chained.has_value() && hexOf(chained->digest) == vectors[k - 1].digestHex, "chained digest of V" + std::to_string(k));
                      }
                      checks.expect(chain.chainedThrough() == 4 && hexOf(chain.headDigest()) == vectors[3].digestHex, "head is H_4");
                      checks.raise();
                  })
            .Then("a refused event leaves the chain where it was",
                  [] {
                      speclab::core::Checks checks;
                      AuditChain            chain{streamName};
                      const auto            skipped = chain.append(eventFor(2));
                      checks.expect(!skipped && skipped.error() == ChainRefusal::OutOfOrder, "sequence 2 first is out of order");
                      AuditEvent other;
                      (void)other.assign({.action = "a", .target = "t"}, "other/stream", 1);
                      const auto foreign = chain.append(other);
                      checks.expect(!foreign && foreign.error() == ChainRefusal::WrongStream, "another stream instance is refused");
                      checks.expect(chain.chainedThrough() == 0 && chain.headDigest() == chainInitialValue, "state unchanged");
                      checks.expect(chain.append(eventFor(1)).has_value(), "the right event is still accepted");
                      AuditChain invalid{"has space"};
                      checks.expect(!invalid.valid() && invalid.append(eventFor(1)).error() == ChainRefusal::WrongStream,
                                    "invalid stream identity chains nothing");
                      checks.raise();
                  })
            .Then("decoding reads back every field, provenance included",
                  [] {
                      speclab::core::Checks checks;
                      const auto            raw  = fromHex(vectors[2].canonicalHex);
                      const auto            read = decodeCanonical(raw);
                      checks.expect(read.status == CanonicalReadStatus::Ok && read.record.sequence == 3 && read.record.detail.size() == 159
                                        && read.record.detailTruncated && read.record.sourceSequence == std::uint64_t{0}
                                        && read.record.time.availability() == mddlog::core::TimeAvailability::Available
                                        && read.record.category == AuditCategory::RiskControl && read.record.phase == AuditPhase::Failed,
                                    "V3 fields");
                      const auto none = decodeCanonical(fromHex(vectors[1].canonicalHex));
                      checks.expect(none.status == CanonicalReadStatus::Ok && !none.record.sourceSequence.has_value()
                                        && none.record.time.availability() == mddlog::core::TimeAvailability::Unavailable,
                                    "absent is not zero (V2)");
                      checks.raise();
                  })
            .Then("tampering, removal, reordering and version faults are each found",
                  [] {
                      speclab::core::Checks                    checks;
                      std::array<std::vector<std::uint8_t>, 4> stored;
                      std::array<Sha256Digest, 4>              digests{};
                      for (std::size_t k = 0; k < 4; ++k) {
                          stored[k]  = fromHex(vectors[k].canonicalHex);
                          digests[k] = digestFromHex(vectors[k].digestHex);
                      }
                      {
                          AuditChainVerifier v{streamName};
                          bool               ok = true;
                          for (std::size_t k = 0; k < 4; ++k)
                              ok = ok && v.check(stored[k], digests[k]) == ChainFinding::Ok;
                          checks.expect(ok && v.verifiedThrough() == 4 && v.headDigest() == digests[3] && !v.failedAt(), "intact chain verifies");
                      }
                      {
                          AuditChainVerifier v{streamName};
                          auto               altered = stored[0];
                          altered.back()             = 0x01;  // detailTruncated flag cleared -> set
                          checks.expect(v.check(altered, digests[0]) == ChainFinding::DigestMismatch && v.failedAt() == 1, "an altered byte breaks the digest");
                          checks.expect(v.check(stored[1], digests[1]) == ChainFinding::DigestMismatch, "a failed chain accepts nothing more");
                      }
                      {
                          AuditChainVerifier v{streamName};
                          checks.expect(v.check(stored[0], digests[0]) == ChainFinding::Ok && v.check(stored[2], digests[2]) == ChainFinding::SequenceBreak,
                                        "a removed record breaks the order");
                      }
                      {
                          AuditChainVerifier bad{std::string(mddlog::core::auditStreamCapacity + 1, 'a')};
                          checks.expect(!bad.valid() && bad.check(stored[0], digests[0]) == ChainFinding::InvalidVerifier && !bad.failedAt(),
                                        "an invalid stream identity is a verifier fault, not a stream finding");
                          AuditChainVerifier v{"other/stream"};
                          checks.expect(v.check(stored[0], digests[0]) == ChainFinding::StreamMismatch, "another stream is found");
                      }
                      {
                          AuditChainVerifier v{streamName, 2, digests[1]};
                          checks.expect(v.check(stored[2], digests[2]) == ChainFinding::Ok, "a trimmed prefix starts from a recorded H");
                      }
                      {
                          auto zero = stored[0];
                          zero[0]   = 0;
                          zero[1]   = 0;
                          AuditChainVerifier v{streamName};
                          checks.expect(v.check(zero, digests[0]) == ChainFinding::Malformed
                                            && v.check(std::span<const std::uint8_t>{}, digests[0]) == ChainFinding::Malformed,
                                        "version 0 is malformed, whatever the reader knows");
                      }
                      {
                          auto v2 = stored[0];
                          v2[1]   = 2;
                          AuditChainVerifier v{streamName};
                          checks.expect(v.check(v2, digests[0]) == ChainFinding::UnsupportedVersion, "an unknown stream version cannot be verified");
                          checks.expect(v.cannotVerify() && !v.failedAt(), "an unknown version alone is Cannot verify");
                          AuditChainVerifier w{streamName};
                          checks.expect(w.check(stored[0], digests[0]) == ChainFinding::Ok && w.check(v2, digests[1]) == ChainFinding::VersionChange,
                                        "a version change inside a stream is inconsistent");
                      }
                      {
                          // 8.3: version 0 and a version change are found even when the stream's version is unknown.
                          auto v2   = stored[0];
                          v2[1]     = 2;
                          auto v3   = stored[1];
                          v3[1]     = 3;
                          auto zero = stored[1];
                          zero[0]   = 0;
                          zero[1]   = 0;
                          AuditChainVerifier a{streamName};
                          checks.expect(a.check(v2, digests[0]) == ChainFinding::UnsupportedVersion && a.check(v3, digests[1]) == ChainFinding::VersionChange,
                                        "2 then 3 is a version change, not a second Cannot verify");
                          checks.expect(a.failedAt() == 2 && !a.cannotVerify(), "the first inconsistent record is named, and Inconsistent wins");
                          AuditChainVerifier b{streamName};
                          checks.expect(b.check(v2, digests[0]) == ChainFinding::UnsupportedVersion && b.check(zero, digests[1]) == ChainFinding::Malformed
                                            && b.failedAt() == 2,
                                        "2 then 0 is malformed");
                          AuditChainVerifier c{streamName};
                          checks.expect(c.check(v2, digests[0]) == ChainFinding::UnsupportedVersion
                                            && c.check(v2, digests[1]) == ChainFinding::UnsupportedVersion && c.cannotVerify() && !c.failedAt(),
                                        "the same unknown version throughout stays Cannot verify");
                      }
                      {
                          auto extra = stored[1];
                          extra.push_back(0);
                          auto bad            = stored[0];
                          bad[2 + 2 + 16 + 8] = 0x09;  // category outside its table
                          AuditChainVerifier v{streamName};
                          checks.expect(v.check(bad, digests[0]) == ChainFinding::Malformed, "an enum byte outside its table is malformed");
                          checks.expect(decodeCanonical(extra).status == CanonicalReadStatus::Malformed, "bytes after field 15 are malformed");
                          checks.expect(decodeCanonical(std::span{stored[0]}.first(stored[0].size() - 1)).status == CanonicalReadStatus::Malformed,
                                        "bytes ending early are malformed");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,hicpp-signed-bitwise)
}  // namespace
