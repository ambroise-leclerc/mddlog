/** @brief Rebuild with mddlog v0.3.0 to regenerate the independent compatibility corpus. */
import std;
import mddlog.core.auditevent;
import mddlog.adapter.auditstore;

namespace {
[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (const auto byte : bytes)
        out += std::format("{:02x}", byte);
    return out;
}
}  // namespace
int main() {
    using namespace mddlog::adapter;
    InMemoryStorageMedium  medium{16, true};
    InMemoryAnchorProvider provider{"v03-witness"};
    StorageConfig          config;
    config.segmentSize        = 2048;
    config.segmentCount       = 16;
    config.maxProducerStreams = 1;
    config.sync.recordBound   = 1;
    config.provider           = &provider;
    config.ledger             = LedgerConfig{.streamId = "v03/ledger", .time = {}};
    const auto sink           = PersistingAuditSink::create(medium, config);
    if (!sink)
        return 1;
    for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
        mddlog::core::AuditEvent event;
        if (!event.assign({.action = "archive.v03", .target = "external-consumer", .detail = "original v0.3 bytes"}, "v03/producer", sequence).wasAdmitted()
            || !(*sink)->accept(event))
            return 1;
    }
    (*sink)->close();
    std::cout << "{\"producerVersion\":\"v0.3.0\",\"segments\":[";
    bool       first     = true;
    const auto inventory = medium.segments();
    if (!inventory)
        return 1;
    for (const auto& segment : *inventory) {
        const auto bytes = medium.read(segment.segment, 0, segment.size);
        if (!bytes)
            return 1;
        if (!std::exchange(first, false))
            std::cout << ',';
        std::cout << std::format("{{\"reference\":{},\"hex\":\"{}\"}}", segment.segment, hex(*bytes));
    }
    const auto listing = std::get<ProviderListing>(provider.streams());
    std::cout << std::format("],\"providerId\":\"{}\",\"head\":{},\"anchors\":[", listing.providerId, listing.head);
    first = true;
    for (const auto& entry : listing.entries) {
        const auto& anchor = std::get<Anchor>(entry);
        if (!std::exchange(first, false))
            std::cout << ',';
        std::cout << std::format("{{\"streamId\":\"{}\",\"position\":{},\"digest\":\"{}\",\"counter\":{},\"canonicalVersion\":{},\"anchorFormat\":{}}}",
                                 anchor.streamId,
                                 anchor.position,
                                 hex(anchor.digest),
                                 anchor.counter,
                                 anchor.canonicalVersion,
                                 anchor.anchorFormat);
    }
    std::cout << "]}\n";
    return std::cout ? 0 : 1;
}
