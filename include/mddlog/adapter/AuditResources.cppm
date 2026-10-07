/** @brief Explicit resource limits for audit readers and the host-owned consumer. */
export module mddlog.adapter.auditresources;
import std;

export namespace mddlog::adapter {

namespace detail {
inline constexpr std::size_t   defaultAuditInventoryEntries  = 4096;
inline constexpr std::uint64_t defaultAuditSegmentBytes      = 1048576;
inline constexpr std::uint64_t defaultAuditTotalBytes        = 67108864;
inline constexpr std::size_t   defaultAuditChunkBytes        = 65536;
inline constexpr std::uint64_t defaultAuditReadBytes         = 134217728;
inline constexpr std::size_t   defaultAuditRecords           = 262144;
inline constexpr std::size_t   defaultAuditProviderTextBytes = 1024;
inline constexpr std::size_t   defaultAuditProviderCalls     = 32768;
}  // namespace detail

/** @brief Finite compatibility defaults. Integrators declare smaller deployment profiles centrally. */
struct AuditResourceLimits {
    std::size_t   maxSegments          = detail::defaultAuditInventoryEntries;
    std::uint64_t maxSegmentBytes      = detail::defaultAuditSegmentBytes;
    std::uint64_t maxTotalBytes        = detail::defaultAuditTotalBytes;
    std::size_t   readChunkBytes       = detail::defaultAuditChunkBytes;
    std::uint64_t maxReadBytes         = detail::defaultAuditReadBytes;
    std::size_t   maxStreams           = detail::defaultAuditInventoryEntries;
    std::size_t   maxRecords           = detail::defaultAuditRecords;
    std::size_t   maxProviderEntries   = detail::defaultAuditInventoryEntries;
    std::size_t   maxProviderTextBytes = detail::defaultAuditProviderTextBytes;
    std::size_t   maxProviderCalls     = detail::defaultAuditProviderCalls;
    std::size_t   maxIntegrityFaults   = detail::defaultAuditInventoryEntries;

    [[nodiscard]] bool valid() const noexcept {
        return maxSegments != 0 && maxSegmentBytes != 0 && maxTotalBytes != 0 && readChunkBytes != 0 && maxReadBytes != 0 && maxStreams != 0 && maxRecords != 0
               && maxProviderEntries != 0 && maxProviderTextBytes != 0 && maxProviderCalls != 0 && maxIntegrityFaults != 0;
    }
};

enum class AuditResourceIssue : std::uint8_t {
    None,
    InvalidLimits,
    Segments,
    SegmentBytes,
    TotalBytes,
    ReadBytes,
    Streams,
    Records,
    ProviderEntries,
    ProviderCalls,
    ProviderTextBytes,
    IntegrityFaults,
    MemoryUnavailable
};

struct AuditResourceUsage {
    std::size_t   segments   = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t bytesRead  = 0;
    std::size_t   readCalls  = 0;
    std::size_t   records    = 0;
};
}  // namespace mddlog::adapter
