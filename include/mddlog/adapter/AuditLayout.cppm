/** @brief Stored-byte layout, version 1: segments, frames, CRC-32C and the reader's scan (ADR-004 Decision 9.4). Adapter zone only. */

export module mddlog.adapter.auditlayout;

import std;
import mddlog.core.auditevent;
export import mddlog.adapter.sha256;
export import mddlog.adapter.auditcanonical;

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic,readability-math-missing-parentheses):
// preamble, frame bytes, CRC polynomial and big-endian shifts are the ADR-004 9.4 wire format itself; reads are bounds-checked against the span before each
// index.
export namespace mddlog::adapter {

/** @brief The layout version this library writes and reads (9.4). Zero is never valid. */
inline constexpr std::uint16_t storageLayoutVersion = 1;

inline constexpr std::array<std::uint8_t, 4> segmentMagic{0x6d, 0x64, 0x6c, 0x67};  // "mdlg"
inline constexpr std::size_t                 segmentPreambleSize = segmentMagic.size() + 2;

inline constexpr std::uint8_t frameTypeHeader = 0x01;
inline constexpr std::uint8_t frameTypeRecord = 0x02;

/** @brief Type, length and check: the bytes a frame adds around its payload. */
inline constexpr std::size_t frameOverhead = 1 + 4 + 4;

inline constexpr std::size_t headerPayloadMin = 15;
inline constexpr std::size_t recordPayloadMin = 1 + sha256DigestSize;  // C_k is at least one byte; 33 in the contract
inline constexpr std::size_t framePayloadMax  = 65'536;

/** @brief Largest preamble plus header frame: 6 + 9 + (4 + 8 + 2 + stream identity at capacity) (9.6, 125 bytes). */
inline constexpr std::size_t maxSegmentOpeningSize = segmentPreambleSize + frameOverhead + 4 + 8 + 2 + core::auditStreamCapacity;
/** @brief Largest record frame of a contract-version-1 record under layout version 1 (9.6, 813 bytes). */
inline constexpr std::size_t maxRecordFrameSize = frameOverhead + canonicalMaxSize + sha256DigestSize;

static_assert(maxSegmentOpeningSize == 125, "ADR-004 9.6 counts the largest preamble and header frame at 125 bytes");
static_assert(maxRecordFrameSize == 813, "ADR-004 9.6 counts the largest record frame at 813 bytes");

namespace detail {

[[nodiscard]] constexpr std::array<std::uint32_t, 256> makeCrc32cTable() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1U) != 0 ? (value >> 1) ^ 0x82F63B78U : value >> 1;
        table[index] = value;
    }
    return table;
}

inline constexpr std::array<std::uint32_t, 256> crc32cTable = makeCrc32cTable();

constexpr void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
constexpr void putU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

[[nodiscard]] constexpr std::uint32_t getU32(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    return (std::uint32_t{bytes[at]} << 24) | (std::uint32_t{bytes[at + 1]} << 16) | (std::uint32_t{bytes[at + 2]} << 8) | std::uint32_t{bytes[at + 3]};
}
[[nodiscard]] constexpr std::uint64_t getU64(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value = (value << 8) | bytes[at + i];
    return value;
}

}  // namespace detail

/**
 * @brief CRC-32C (Castagnoli) of `data`, continuing from `crc`, the value returned for the bytes before them (0 to start).
 *
 * Reflected polynomial 0x82F63B78, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF; the check value of the ASCII bytes "123456789" is
 * 0xE3069283 (9.4). It detects accidental damage such as a torn write and proves nothing against a deliberate rewrite.
 */
[[nodiscard]] constexpr std::uint32_t crc32c(std::span<const std::uint8_t> data, std::uint32_t crc = 0) noexcept {
    std::uint32_t state = ~crc;
    for (const std::uint8_t byte : data)
        state = detail::crc32cTable[(state ^ byte) & 0xFFU] ^ (state >> 8);
    return ~state;
}

/** @brief One frame: type, length, payload (the parts back to back) and the CRC-32C of those bytes (9.4). */
[[nodiscard]] inline std::vector<std::uint8_t> encodeFrame(std::uint8_t type, std::span<const std::uint8_t> first, std::span<const std::uint8_t> second = {}) {
    std::vector<std::uint8_t> out;
    out.reserve(frameOverhead + first.size() + second.size());
    out.push_back(type);
    detail::putU32(out, static_cast<std::uint32_t>(first.size() + second.size()));
    out.insert(out.end(), first.begin(), first.end());
    out.insert(out.end(), second.begin(), second.end());
    detail::putU32(out, crc32c(out));
    return out;
}

/** @brief The record frame of one chained record: payload C_k then H_k (9.4). */
[[nodiscard]] inline std::vector<std::uint8_t> encodeRecordFrame(std::span<const std::uint8_t> canonical, const Sha256Digest& digest) {
    return encodeFrame(frameTypeRecord, canonical, digest);
}

/** @brief Size of the record frame holding `canonicalSize` canonical bytes. */
[[nodiscard]] constexpr std::size_t recordFrameSize(std::size_t canonicalSize) noexcept {
    return frameOverhead + canonicalSize + sha256DigestSize;
}

/**
 * @brief Preamble and header frame of a new segment (9.4): what `open` appends, and what a medium must hold first.
 *
 * Empty when `streamId` is not a stream identity AuditEvent accepts.
 */
[[nodiscard]] inline std::vector<std::uint8_t> encodeSegmentOpening(std::string_view streamId, std::uint32_t segmentIndex, std::uint64_t firstSequence) {
    if (!core::AuditEvent::validStreamId(streamId))
        return {};
    std::vector<std::uint8_t> out{segmentMagic.begin(), segmentMagic.end()};
    out.push_back(static_cast<std::uint8_t>(storageLayoutVersion >> 8));
    out.push_back(static_cast<std::uint8_t>(storageLayoutVersion));
    std::vector<std::uint8_t> payload;
    detail::putU32(payload, segmentIndex);
    detail::putU64(payload, firstSequence);
    payload.push_back(static_cast<std::uint8_t>(streamId.size() >> 8));
    payload.push_back(static_cast<std::uint8_t>(streamId.size()));
    for (const char ch : streamId)
        payload.push_back(static_cast<std::uint8_t>(ch));
    const auto frame = encodeFrame(frameTypeHeader, payload);
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

/** @brief The segment header a reader decoded (9.4). */
struct SegmentHeader {
    std::uint32_t segmentIndex  = 0;
    std::uint64_t firstSequence = 0;
    std::string   streamId;
};

/** @brief One valid record frame: where it starts, its canonical bytes C_k and its stored digest H_k. */
struct ScannedRecord {
    std::size_t                   frameOffset = 0;
    std::span<const std::uint8_t> canonical;
    Sha256Digest                  digest{};
};

enum class SegmentStatus : std::uint8_t {
    /** @brief A valid preamble and header frame. The records found are the valid frames after it. */
    Readable,
    /** @brief Short, wrong magic, or layout version 0: nothing is read from the segment. */
    NoValidPreamble,
    /** @brief The preamble is valid but its layout version is not one this reader knows: nothing is read, and the stream cannot be verified (9.4). */
    UnknownLayoutVersion,
    /** @brief A valid preamble without a valid header frame, as a cut during `open` leaves: nothing is read. */
    NoValidHeader
};

/** @brief What a reader concludes from one segment's bytes (9.4, "Recognising a partial record"). */
struct SegmentScan {
    SegmentStatus                status        = SegmentStatus::NoValidPreamble;
    std::uint16_t                layoutVersion = 0;
    std::optional<SegmentHeader> header;
    /** @brief Valid record frames in storage order. They point into the bytes that were scanned. */
    std::vector<ScannedRecord> records;
    /** @brief Offset just after the last valid frame (the end of the preamble when no frame is valid). */
    std::size_t validEnd = 0;
    /** @brief Bytes from validEnd to the end of the segment: unused space, or the trace of a cut. Never parsed. */
    std::size_t trailingBytes       = 0;
    bool        recordLimitExceeded = false;
};

namespace detail {

struct FrameView {
    std::uint8_t                  type = 0;
    std::span<const std::uint8_t> payload;
    std::size_t                   end = 0;
};

/** @brief The five rules of 9.4 at offset `at`; empty when any fails, where the scan stops. */
[[nodiscard]] inline std::optional<FrameView> parseFrame(std::span<const std::uint8_t> bytes, std::size_t at, bool firstFrame) noexcept {
    if (at > bytes.size() || bytes.size() - at < 5)  // rule 1
        return std::nullopt;
    const std::uint8_t type = bytes[at];
    if (type != (firstFrame ? frameTypeHeader : frameTypeRecord))  // rule 2
        return std::nullopt;
    const std::size_t length = getU32(bytes, at + 1);
    const std::size_t lower  = firstFrame ? headerPayloadMin : recordPayloadMin;
    if (length < lower || length > framePayloadMax)  // rule 3
        return std::nullopt;
    if (bytes.size() - at - 5 < length + 4)          // rule 4
        return std::nullopt;
    const std::size_t checkAt = at + 5 + length;
    if (crc32c(bytes.subspan(at, 5 + length)) != getU32(bytes, checkAt))  // rule 5
        return std::nullopt;
    return FrameView{.type = type, .payload = bytes.subspan(at + 5, length), .end = checkAt + 4};
}

[[nodiscard]] inline std::optional<SegmentHeader> decodeHeaderPayload(std::span<const std::uint8_t> payload) {
    if (payload.size() < headerPayloadMin)
        return std::nullopt;
    const std::size_t nameLength = (std::size_t{payload[12]} << 8) | payload[13];
    if (payload.size() != 14 + nameLength)
        return std::nullopt;
    const std::string_view name{reinterpret_cast<const char*>(payload.data()) + 14, nameLength};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!core::AuditEvent::validStreamId(name))
        return std::nullopt;
    return SegmentHeader{.segmentIndex = getU32(payload, 0), .firstSequence = getU64(payload, 4), .streamId = std::string{name}};
}

}  // namespace detail

/**
 * @brief Scan one segment from the end of its preamble and stop at the first byte that is not a valid frame (9.4).
 *
 * It never parses a record from the trailing bytes, never repairs them and never resynchronises inside them. The records point into `bytes`,
 * which must outlive the result.
 */
[[nodiscard]] inline SegmentScan scanSegment(std::span<const std::uint8_t> bytes, std::size_t maxRecords = std::numeric_limits<std::size_t>::max()) {
    SegmentScan scan;
    if (bytes.size() < segmentPreambleSize || !std::ranges::equal(bytes.first(segmentMagic.size()), segmentMagic)) {
        scan.trailingBytes = bytes.size();
        return scan;
    }
    scan.layoutVersion = static_cast<std::uint16_t>((std::uint16_t{bytes[4]} << 8) | bytes[5]);
    if (scan.layoutVersion == 0) {
        scan.trailingBytes = bytes.size();
        return scan;
    }
    scan.validEnd = segmentPreambleSize;
    if (scan.layoutVersion != storageLayoutVersion) {
        scan.status        = SegmentStatus::UnknownLayoutVersion;
        scan.trailingBytes = bytes.size() - scan.validEnd;
        return scan;
    }
    scan.status            = SegmentStatus::NoValidHeader;
    scan.trailingBytes     = bytes.size() - scan.validEnd;
    const auto headerFrame = detail::parseFrame(bytes, scan.validEnd, true);
    if (!headerFrame)
        return scan;
    auto header = detail::decodeHeaderPayload(headerFrame->payload);
    if (!header)
        return scan;
    scan.header   = std::move(header);
    scan.status   = SegmentStatus::Readable;
    scan.validEnd = headerFrame->end;
    for (;;) {
        const auto frame = detail::parseFrame(bytes, scan.validEnd, false);
        if (!frame)
            break;
        if (scan.records.size() == maxRecords) {
            scan.recordLimitExceeded = true;
            break;
        }
        ScannedRecord record{.frameOffset = scan.validEnd, .canonical = frame->payload.first(frame->payload.size() - sha256DigestSize)};
        std::ranges::copy(frame->payload.last(sha256DigestSize), record.digest.begin());
        scan.records.push_back(record);
        scan.validEnd = frame->end;
    }
    scan.trailingBytes = bytes.size() - scan.validEnd;
    return scan;
}

}  // namespace mddlog::adapter
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic,readability-math-missing-parentheses)
