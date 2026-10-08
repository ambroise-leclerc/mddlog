/** @brief Versioned evidence packages with bounded decoding and read-only replay (#119). */
export module mddlog.adapter.auditevidence;
import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditlogverifier;
import mddlog.adapter.witnesscodec;

export namespace mddlog::adapter {
inline constexpr std::uint64_t auditEvidenceVersion = 1;
inline constexpr std::size_t   defaultEvidenceBytes = std::size_t{144} * 1024 * 1024;

struct EvidenceSegment {
    SegmentRef                reference = 0;
    std::vector<std::uint8_t> bytes;
};
/** @brief Embedded positions and witness data are evidence inputs, never self-authenticating. */
struct AuditEvidence {
    std::string                    source;
    std::string                    providerProvenance;
    std::optional<ProviderListing> provider;
    RetainedPosition               retained;
    VerifierConfig                 verification;
    std::vector<EvidenceSegment>   segments;
};

/** @brief Refuses every mutation; the evidence owner must outlive the medium. */
class EvidenceMedium final : public StorageMedium {
public:
    explicit EvidenceMedium(const AuditEvidence& evidence) : held(&evidence) {}
    [[nodiscard]] OpenAnswer open(const SegmentOpening&) override {
        return {};
    }
    [[nodiscard]] AppendAnswer append(SegmentRef, std::span<const std::uint8_t>) override {
        return {};
    }
    [[nodiscard]] SyncAnswer sync(SegmentRef, std::uint64_t) override {
        return SyncAnswer::Unsupported;
    }
    [[nodiscard]] bool reclaim(SegmentRef) override {
        return false;
    }
    [[nodiscard]] std::optional<std::vector<SegmentInfo>> segments() override {
        std::vector<SegmentInfo> out;
        out.reserve(held->segments.size());
        for (const auto& segment : held->segments)
            out.push_back({segment.reference, segment.bytes.size()});
        return out;
    }
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(SegmentRef reference, std::uint64_t offset, std::uint64_t length) override {
        const auto found = std::ranges::find(held->segments, reference, &EvidenceSegment::reference);
        if (found == held->segments.end())
            return std::nullopt;
        if (offset >= found->bytes.size())
            return std::vector<std::uint8_t>{};
        const auto bytes =
            std::span(found->bytes).subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(std::min(length, found->bytes.size() - offset)));
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }

private:
    const AuditEvidence* held;
};

/** @brief A frozen provider listing, or unavailable. Acceptance of its provenance belongs to the caller. */
class EvidenceProvider final : public AnchorProvider {
public:
    explicit EvidenceProvider(const std::optional<ProviderListing>& listing) : held(&listing) {}
    [[nodiscard]] AdvanceAnswer advance(const AnchorClaim&) override {
        return ProviderUnavailable{};
    }
    [[nodiscard]] RetireAnswer retire(std::string_view, std::uint64_t) override {
        return ProviderUnavailable{};
    }
    [[nodiscard]] StreamsAnswer streams() override {
        return *held ? StreamsAnswer{**held} : StreamsAnswer{ProviderUnavailable{}};
    }
    [[nodiscard]] LatestAnswer latest(std::string_view id) override {
        if (!*held)
            return ProviderUnavailable{};
        for (const auto& entry : (**held).entries) {
            if (witness::anchorOf(entry).streamId == id)
                return std::visit(
                    [](const auto& value) -> LatestAnswer {
                        return value;
                    },
                    entry);
        }
        return AnchorAbsent{};
    }

private:
    const std::optional<ProviderListing>* held;
};

/** @brief Capture every byte under reader budgets. Call with an exclusively stable/read-locked medium. */
[[nodiscard]] inline AuditEvidence captureAuditEvidence(StorageMedium& medium, std::string source, VerifierConfig config = {}) {
    AuditReadSession session(config.resources);
    const auto       listing = session.inventory(medium);
    if (!listing)
        throw std::runtime_error(std::format("cannot capture inventory: resource issue {}", std::to_underlying(session.issue())));
    AuditEvidence out;
    out.source             = std::move(source);
    out.providerProvenance = "unavailable";
    out.verification       = config;
    std::set<SegmentRef> seen;
    for (const auto& segment : *listing) {
        if (segment.segment == 0 || !seen.insert(segment.segment).second)
            throw std::invalid_argument("duplicate or zero segment reference");
        auto bytes = session.read(medium, segment);
        if (!bytes)
            throw std::runtime_error(std::format("cannot capture segment {}: resource issue {}", segment.segment, std::to_underlying(session.issue())));
        out.segments.push_back({segment.segment, std::move(*bytes)});
    }
    return out;
}

namespace evidencecodec {
inline constexpr std::size_t                 profileFields = 12;
inline constexpr std::array<std::uint8_t, 8> magic{'M', 'D', 'D', 'A', 'U', 'D', 'I', 'T'};
inline constexpr unsigned                    byteBits      = 8;
inline constexpr std::size_t                 metadataBytes = std::size_t{1024} * 1024;

inline void number(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < sizeof(value); ++i) {
        out.push_back(static_cast<std::uint8_t>(value));
        value >>= byteBits;
    }
}
inline void blob(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes) {
    number(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}
inline void text(std::vector<std::uint8_t>& out, std::string_view value) {
    number(out, value.size());
    for (const char byte : value)
        out.push_back(static_cast<std::uint8_t>(byte));
}
[[nodiscard]] inline auto values(const AuditResourceLimits& limits) {
    return std::array<std::uint64_t, profileFields>{limits.maxSegments,
                                                    limits.maxSegmentBytes,
                                                    limits.maxTotalBytes,
                                                    limits.readChunkBytes,
                                                    limits.maxReadBytes,
                                                    limits.maxStreams,
                                                    limits.maxRecords,
                                                    limits.maxProviderEntries,
                                                    limits.maxProviderTextBytes,
                                                    limits.maxProviderCalls,
                                                    limits.maxIntegrityFaults,
                                                    0};
}
inline void limitsFrom(AuditResourceLimits& limits, const std::array<std::uint64_t, profileFields>& value) {
    if (std::ranges::any_of(value,
                            [](auto item) {
                                return item > std::numeric_limits<std::size_t>::max();
                            })
        || value.back() != 0)
        throw std::invalid_argument("invalid resource profile");
    const auto& [segments, segmentBytes, totalBytes, chunkBytes, readBytes, streams, records, entries, textBytes, calls, faults, reserved] = value;
    (void)reserved;
    limits = {.maxSegments          = static_cast<std::size_t>(segments),
              .maxSegmentBytes      = segmentBytes,
              .maxTotalBytes        = totalBytes,
              .readChunkBytes       = static_cast<std::size_t>(chunkBytes),
              .maxReadBytes         = readBytes,
              .maxStreams           = static_cast<std::size_t>(streams),
              .maxRecords           = static_cast<std::size_t>(records),
              .maxProviderEntries   = static_cast<std::size_t>(entries),
              .maxProviderTextBytes = static_cast<std::size_t>(textBytes),
              .maxProviderCalls     = static_cast<std::size_t>(calls),
              .maxIntegrityFaults   = static_cast<std::size_t>(faults)};
    if (!limits.valid())
        throw std::invalid_argument("invalid resource profile");
}
class Decoder {
public:
    explicit Decoder(std::span<const std::uint8_t> input) : bytes(input) {}
    [[nodiscard]] std::span<const std::uint8_t> raw(std::size_t size) {
        if (size > bytes.size() - offset)
            throw std::invalid_argument("truncated evidence archive");
        const auto out = bytes.subspan(offset, size);
        offset        += size;
        return out;
    }
    [[nodiscard]] std::uint64_t number() {
        const auto    data = raw(sizeof(std::uint64_t));
        std::uint64_t out  = 0;
        for (unsigned i = 0; i < data.size(); ++i)
            out |= std::uint64_t{data[i]} << (byteBits * i);
        return out;
    }
    [[nodiscard]] std::span<const std::uint8_t> blob(std::uint64_t maximum) {
        const auto size = number();
        if (size > maximum || size > std::numeric_limits<std::size_t>::max())
            throw std::length_error("evidence field exceeds budget");
        return raw(static_cast<std::size_t>(size));
    }
    [[nodiscard]] std::string text(std::size_t maximum) {
        const auto data = blob(maximum);
        return {data.begin(), data.end()};
    }
    void end() const {
        if (offset != bytes.size())
            throw std::invalid_argument("trailing archive bytes");
    }

private:
    std::span<const std::uint8_t> bytes;
    std::size_t                   offset = 0;
};
}  // namespace evidencecodec

/** @brief Decode v1 only. Local budgets cap all embedded budgets, and are checked before copying fields. */
[[nodiscard]] inline AuditEvidence
decodeAuditEvidence(std::span<const std::uint8_t> bytes, AuditResourceLimits local = {}, std::size_t maxBytes = defaultEvidenceBytes) {
    using namespace evidencecodec;
    if (!local.valid())
        throw std::invalid_argument("invalid local evidence limits");
    if (bytes.size() > maxBytes)
        throw std::length_error("archive exceeds byte budget");
    if (bytes.size() < magic.size() + sizeof(std::uint64_t) + sha256DigestSize || !std::ranges::equal(bytes.first(magic.size()), magic))
        throw std::invalid_argument("not an evidence package; projections cannot be imported");
    const auto payload = bytes.first(bytes.size() - sha256DigestSize);
    Decoder    input(payload);
    (void)input.raw(magic.size());
    if (input.number() != auditEvidenceVersion)
        throw std::invalid_argument("unsupported evidence version");
    if (!std::ranges::equal(sha256(payload), bytes.last(sha256DigestSize)))
        throw std::invalid_argument("evidence checksum mismatch or truncation");
    AuditEvidence out;
    out.source                 = input.text(local.maxProviderTextBytes);
    out.providerProvenance     = input.text(local.maxProviderTextBytes);
    const auto       timeBytes = input.blob(metadataBytes);
    witness::Decoder clock(timeBytes);
    out.verification.verificationTime = clock.time();
    clock.end();
    const auto age = input.number();
    if (age > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid anchor age");
    if (age != 0)
        out.verification.maxAnchorAge = std::chrono::nanoseconds{static_cast<std::int64_t>(age - 1)};
    std::array<std::uint64_t, profileFields> profile{};
    const auto                               maxima = values(local);
    for (std::size_t i = 0; i < profile.size(); ++i) {
        std::span{profile}[i] = input.number();
        if (std::span{profile}[i] > std::span{maxima}[i])
            throw std::length_error("embedded resource profile exceeds local budget");
    }
    limitsFrom(out.verification.resources, profile);
    const auto& limits        = out.verification.resources;
    const auto  providerBytes = input.blob(metadataBytes);
    if (!providerBytes.empty()) {
        witness::Decoder providerInput(providerBytes);
        out.provider = providerInput.listing();
        providerInput.end();
        if (!witness::validListing(*out.provider, out.provider->providerId) || out.provider->entries.size() > limits.maxProviderEntries
            || out.provider->providerId.size() > limits.maxProviderTextBytes)
            throw std::invalid_argument("invalid or oversized witness snapshot");
        for (const auto& entry : out.provider->entries) {
            if (witness::anchorOf(entry).streamId.size() > limits.maxProviderTextBytes)
                throw std::length_error("witness identity exceeds budget");
        }
    }
    const auto heads = input.number();
    if (heads > limits.maxProviderEntries)
        throw std::length_error("retained heads exceed budget");
    for (std::uint64_t i = 0; i < heads; ++i) {
        const auto id = input.text(limits.maxProviderTextBytes);
        if (id.empty() || id.contains('\0') || out.retained.head(id))
            throw std::invalid_argument("invalid retained head");
        out.retained.raiseHead(id, input.number());
    }
    const auto anchors = input.number();
    if (anchors > limits.maxProviderEntries)
        throw std::length_error("retained anchors exceed budget");
    for (std::uint64_t i = 0; i < anchors; ++i) {
        const auto     id = input.text(limits.maxProviderTextBytes);
        RetainedAnchor anchor{.position = input.number(), .counter = input.number()};
        const auto     retired = input.number();
        if (retired > 1 || !core::AuditEvent::validStreamId(id) || out.retained.anchor(id) || anchor.position == 0 || anchor.counter == 0)
            throw std::invalid_argument("invalid retained anchor");
        anchor.retired = retired != 0;
        std::ranges::copy(input.raw(sha256DigestSize), anchor.digest.begin());
        out.retained.raiseAnchor(id, anchor);
    }
    const auto count = input.number();
    if (count > limits.maxSegments)
        throw std::length_error("segment inventory exceeds budget");
    std::set<SegmentRef> seen;
    std::uint64_t        total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto reference = input.number();
        if (reference == 0 || !seen.insert(reference).second)
            throw std::invalid_argument("duplicate or zero segment reference");
        const auto data = input.blob(std::min(limits.maxSegmentBytes, limits.maxTotalBytes - total));
        total          += data.size();
        out.segments.push_back({
            reference,
            {data.begin(), data.end()}
        });
    }
    input.end();
    return out;
}

/** @brief Encode raw segments and verification inputs; the final SHA-256 is a checksum, not authentication. */
[[nodiscard]] inline std::vector<std::uint8_t> encodeAuditEvidence(const AuditEvidence& evidence, std::size_t maxBytes = defaultEvidenceBytes) {
    using namespace evidencecodec;
    const auto& limits = evidence.verification.resources;
    if (!limits.valid() || evidence.source.size() > limits.maxProviderTextBytes || evidence.providerProvenance.size() > limits.maxProviderTextBytes
        || evidence.retained.allHeads().size() > limits.maxProviderEntries || evidence.retained.allAnchors().size() > limits.maxProviderEntries
        || evidence.segments.size() > limits.maxSegments)
        throw std::length_error("evidence metadata exceeds resource profile");
    std::size_t estimated = magic.size() + sha256DigestSize;
    const auto  charge    = [&](std::size_t amount) {
        if (estimated > maxBytes || amount > maxBytes - estimated)
            throw std::length_error("archive exceeds byte budget");
        estimated += amount;  // NOLINT(clang-analyzer-deadcode.DeadStores): updated reference capture feeds later budget charges and reserve.
    };
    charge(evidence.source.size());
    charge(evidence.providerProvenance.size());
    for (const auto& [id, head] : evidence.retained.allHeads()) {
        (void)head;
        if (id.size() > limits.maxProviderTextBytes)
            throw std::length_error("retained identity exceeds budget");
        charge(id.size());
    }
    for (const auto& [id, anchor] : evidence.retained.allAnchors()) {
        (void)anchor;
        if (id.size() > limits.maxProviderTextBytes)
            throw std::length_error("retained identity exceeds budget");
        charge(id.size());
    }
    std::uint64_t total = 0;
    for (const auto& segment : evidence.segments) {
        if (segment.bytes.size() > limits.maxSegmentBytes || segment.bytes.size() > limits.maxTotalBytes - total)
            throw std::length_error("segment bytes exceed resource profile");
        total += segment.bytes.size();
        charge(segment.bytes.size());
    }
    if (evidence.provider
        && (evidence.provider->entries.size() > limits.maxProviderEntries || !witness::validListing(*evidence.provider, evidence.provider->providerId)))
        throw std::invalid_argument("invalid or oversized witness snapshot");
    std::vector<std::uint8_t> out(magic.begin(), magic.end());
    out.reserve(estimated);
    number(out, auditEvidenceVersion);
    text(out, evidence.source);
    text(out, evidence.providerProvenance);
    witness::Encoder clock;
    clock.time(evidence.verification.verificationTime);
    blob(out, clock.bytes);
    const auto age = evidence.verification.maxAnchorAge;
    if (age && (age->count() < 0 || age->count() == std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid anchor age");
    number(out, age ? static_cast<std::uint64_t>(age->count()) + 1 : 0);
    for (const auto value : values(evidence.verification.resources))
        number(out, value);
    witness::Encoder provider;
    if (evidence.provider)
        provider.listing(*evidence.provider);
    blob(out, provider.bytes);
    number(out, evidence.retained.allHeads().size());
    for (const auto& [id, head] : evidence.retained.allHeads()) {
        text(out, id);
        number(out, head);
    }
    number(out, evidence.retained.allAnchors().size());
    for (const auto& [id, anchor] : evidence.retained.allAnchors()) {
        text(out, id);
        number(out, anchor.position);
        number(out, anchor.counter);
        number(out, anchor.retired ? 1 : 0);
        out.insert(out.end(), anchor.digest.begin(), anchor.digest.end());
    }
    number(out, evidence.segments.size());
    for (const auto& segment : evidence.segments) {
        if (out.size() > maxBytes || segment.bytes.size() > maxBytes - out.size()
            || maxBytes - out.size() - segment.bytes.size() < (2 * sizeof(std::uint64_t)) + sha256DigestSize)
            throw std::length_error("archive exceeds byte budget");
        number(out, segment.reference);
        blob(out, segment.bytes);
    }
    if (out.size() > maxBytes || maxBytes - out.size() < sha256DigestSize)
        throw std::length_error("archive exceeds byte budget");
    const auto checksum = sha256(out);
    out.insert(out.end(), checksum.begin(), checksum.end());
    (void)decodeAuditEvidence(out, evidence.verification.resources, maxBytes);
    return out;
}
}  // namespace mddlog::adapter
