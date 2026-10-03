/** @brief Canonical byte contract, version 1, for AuditEvent (ADR-004 Decision 8). */

export module mddlog.adapter.auditcanonical;

import std;
import mddlog.core.auditevent;

export namespace mddlog::adapter {

/** @brief The contract version this encoder writes (ADR-004 8.1). Version 0 is never valid. */
inline constexpr std::uint16_t canonicalContractVersion = 1;

/** @brief Largest canonical record of version 1: every field at its AuditEvent capacity (8.2). */
inline constexpr std::size_t canonicalMaxSize = 2                                     // contract version
                                                + 2 + core::auditStreamCapacity       // streamId
                                                + 8                                   // sequence
                                                + 1 + 1                               // category, phase
                                                + 1 + 8                               // time
                                                + 2 + core::auditActionCapacity       // action
                                                + 2 + core::auditActorCapacity        // actor
                                                + 2 + core::auditTargetCapacity       // target
                                                + 2 + core::auditReferenceCapacity    // requirementRef
                                                + 2 + core::auditReferenceCapacity    // riskRef
                                                + 2 + core::auditCorrelationCapacity  // correlationId
                                                + 1 + 8                               // sourceSequence
                                                + 2 + core::auditDetailCapacity       // detail
                                                + 1;                                  // detailTruncated

// The i64 time field takes the nanosecond count as RawTime holds it, with no conversion (8.2).
// Width and signedness, not type identity: int64_t is `long` on Linux but nanoseconds::rep is `long long`.
static_assert(std::is_signed_v<std::chrono::nanoseconds::rep> && sizeof(std::chrono::nanoseconds::rep) == sizeof(std::int64_t),
              "canonical contract version 1 requires 64-bit signed nanoseconds");

namespace detail {

[[nodiscard]] constexpr std::uint8_t categoryByte(core::AuditCategory value) noexcept {
    switch (value) {
        case core::AuditCategory::Lifecycle:
            return 0x01;
        case core::AuditCategory::Configuration:
            return 0x02;
        case core::AuditCategory::Access:
            return 0x03;
        case core::AuditCategory::RiskControl:
            return 0x04;
        case core::AuditCategory::Operator:
            return 0x05;
    }
    return 0x00;  // not a valid enum byte; the encoder refuses it
}

[[nodiscard]] constexpr std::uint8_t phaseByte(core::AuditPhase value) noexcept {
    switch (value) {
        case core::AuditPhase::Requested:
            return 0x01;
        case core::AuditPhase::Confirmed:
            return 0x02;
        case core::AuditPhase::Executed:
            return 0x03;
        case core::AuditPhase::Failed:
            return 0x04;
    }
    return 0x00;
}

class Writer {
public:
    constexpr explicit Writer(std::span<std::uint8_t> target) noexcept : out(target) {}

    constexpr void u8(std::uint8_t value) noexcept {
        out[used++] = value;
    }
    constexpr void u16(std::uint16_t value) noexcept {
        u8(static_cast<std::uint8_t>(value >> 8));
        u8(static_cast<std::uint8_t>(value));
    }
    constexpr void u64(std::uint64_t value) noexcept {
        for (int shift = 56; shift >= 0; shift -= 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    constexpr void string(std::string_view value) noexcept {
        u16(static_cast<std::uint16_t>(value.size()));
        for (const char ch : value)
            u8(static_cast<std::uint8_t>(ch));
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return used;
    }

private:
    std::span<std::uint8_t> out;
    std::size_t             used = 0;
};

}  // namespace detail

/** @brief Owned canonical bytes C of one record; no digest, frame or signature is part of it. */
class CanonicalRecord {
public:
    [[nodiscard]] constexpr std::span<const std::uint8_t> bytes() const noexcept {
        return std::span<const std::uint8_t>{storage}.first(length);
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return length;
    }

    /** @brief See encodeCanonical(). */
    [[nodiscard]] static constexpr std::optional<CanonicalRecord> encode(const core::AuditEvent& event) noexcept {
        const std::uint8_t category = detail::categoryByte(event.category());
        const std::uint8_t phase    = detail::phaseByte(event.phase());
        if (category == 0x00 || phase == 0x00)
            return std::nullopt;
        CanonicalRecord record;
        detail::Writer  w{record.storage};
        w.u16(canonicalContractVersion);
        w.string(event.streamId());
        w.u64(event.sequence());
        w.u8(category);
        w.u8(phase);
        if (event.time().availability() == core::TimeAvailability::Available) {
            w.u8(0x01);
            w.u64(static_cast<std::uint64_t>(event.time().value().time_since_epoch().count()));
        } else {
            w.u8(0x00);
        }
        w.string(event.action());
        w.string(event.actor());
        w.string(event.target());
        w.string(event.requirementRef());
        w.string(event.riskRef());
        w.string(event.correlationId());
        if (const auto source = event.sourceSequence(); source.has_value()) {
            w.u8(0x01);
            w.u64(*source);
        } else {
            w.u8(0x00);
        }
        w.string(event.detail());
        w.u8(event.detailTruncated() ? 0x01 : 0x00);
        record.length = w.size();
        return record;
    }

private:
    std::array<std::uint8_t, canonicalMaxSize> storage{};
    std::size_t                                length = 0;
};


/**
 * @brief Encode every field of the event, in contract order, exactly as the event holds it.
 *
 * Nothing is truncated, validated or repaired here: truncation happened at admission and the
 * stored detailTruncated flag is encoded as is (8.2). Returns nullopt only when an enumerator
 * has no byte in the contract tables, which an event built by AuditEvent::assign never has.
 */
[[nodiscard]] constexpr std::optional<CanonicalRecord> encodeCanonical(const core::AuditEvent& event) noexcept {
    return CanonicalRecord::encode(event);
}

/** @brief Outcome of reading stored bytes (ADR-004 8.3). */
enum class CanonicalReadStatus : std::uint8_t {
    Ok,
    /** @brief Shorter than the version bytes, or version 0, or a field rule below failed. */
    Malformed,
    /** @brief Well-formed version bytes naming a version this reader does not implement. */
    UnsupportedVersion
};

/** @brief Fields of a decoded record; the string views point into the bytes that were read. */
struct DecodedAuditRecord {
    std::uint16_t                version = 0;
    std::string_view             streamId;
    std::uint64_t                sequence = 0;
    core::AuditCategory          category = core::AuditCategory::Lifecycle;
    core::AuditPhase             phase    = core::AuditPhase::Requested;
    core::RawTime                time     = core::RawTime::unavailable();
    std::string_view             action;
    std::string_view             actor;
    std::string_view             target;
    std::string_view             requirementRef;
    std::string_view             riskRef;
    std::string_view             correlationId;
    std::optional<std::uint64_t> sourceSequence;
    std::string_view             detail;
    bool                         detailTruncated = false;
};

struct CanonicalReadResult {
    CanonicalReadStatus status = CanonicalReadStatus::Malformed;
    DecodedAuditRecord  record = {};
    /** @brief Version bytes when at least two bytes exist, else 0; meaningful for any status. */
    std::uint16_t version = 0;
};

/** @brief Version of a stored record, or 0 when fewer than two bytes exist (8.3 step 1). */
[[nodiscard]] constexpr std::uint16_t canonicalVersionOf(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < 2)
        return 0;
    return static_cast<std::uint16_t>((std::uint16_t{bytes[0]} << 8) | bytes[1]);
}

namespace detail {

[[nodiscard]] constexpr bool identifierByte(std::uint8_t b) noexcept {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_' || b == '.' || b == ':' || b == '/' || b == '-';
}

class Reader {
public:
    constexpr explicit Reader(std::span<const std::uint8_t> source) noexcept : in(source) {}

    [[nodiscard]] constexpr bool u8(std::uint8_t& value) noexcept {
        if (in.size() - used < 1)
            return false;
        value = in[used++];
        return true;
    }
    [[nodiscard]] constexpr bool u64(std::uint64_t& value) noexcept {
        if (in.size() - used < 8)
            return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value = (value << 8) | in[used++];
        return true;
    }
    /** @brief Read a counted string no longer than capacity, optionally restricted to the identifier grammar. */
    [[nodiscard]] constexpr bool string(std::string_view& value, std::size_t capacity, bool identifier, bool required) noexcept {
        if (in.size() - used < 2)
            return false;
        const std::size_t count = (std::size_t{in[used]} << 8) | in[used + 1];
        used                   += 2;
        if (count > capacity || (required && count == 0) || in.size() - used < count)
            return false;
        for (std::size_t i = 0; i < count; ++i)
            if (identifier && !identifierByte(in[used + i]))
                return false;
        value = std::string_view{reinterpret_cast<const char*>(in.data()) + used, count};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        used += count;
        return true;
    }
    [[nodiscard]] constexpr bool atEnd() const noexcept {
        return used == in.size();
    }

private:
    std::span<const std::uint8_t> in;
    std::size_t                   used = 0;
};

}  // namespace detail

/**
 * @brief Decode one stored record under version 1 and apply every check of 8.3.
 *
 * The caller passes exactly one record's bytes (the storage frame is Decision 9). A version other
 * than 1 is reported without decoding anything; version 0 and short input are Malformed whatever
 * the reader supports.
 */
[[nodiscard]] constexpr CanonicalReadResult decodeCanonical(std::span<const std::uint8_t> bytes) noexcept {
    CanonicalReadResult result;
    result.version = canonicalVersionOf(bytes);
    if (bytes.size() < 2 || result.version == 0)
        return result;
    if (result.version != canonicalContractVersion) {
        result.status = CanonicalReadStatus::UnsupportedVersion;
        return result;
    }

    using detail::Reader;
    Reader             r{bytes.subspan(2)};
    DecodedAuditRecord d;
    d.version               = result.version;
    std::uint8_t  cat       = 0;
    std::uint8_t  pha       = 0;
    std::uint8_t  timeP     = 0;
    std::uint8_t  srcP      = 0;
    std::uint8_t  trunc     = 0;
    std::uint64_t timeValue = 0;
    std::uint64_t srcValue  = 0;
    if (!r.string(d.streamId, core::auditStreamCapacity, true, true) || !r.u64(d.sequence) || d.sequence == 0 || !r.u8(cat) || !r.u8(pha) || !r.u8(timeP)
        || timeP > 1 || (timeP == 1 && !r.u64(timeValue)) || !r.string(d.action, core::auditActionCapacity, true, true)
        || !r.string(d.actor, core::auditActorCapacity, true, false) || !r.string(d.target, core::auditTargetCapacity, true, true)
        || !r.string(d.requirementRef, core::auditReferenceCapacity, true, false) || !r.string(d.riskRef, core::auditReferenceCapacity, true, false)
        || !r.string(d.correlationId, core::auditCorrelationCapacity, true, false) || !r.u8(srcP) || srcP > 1 || (srcP == 1 && !r.u64(srcValue))
        || !r.string(d.detail, core::auditDetailCapacity, false, false) || !r.u8(trunc) || trunc > 1 || !r.atEnd())
        return result;

    switch (cat) {
        case 0x01:
            d.category = core::AuditCategory::Lifecycle;
            break;
        case 0x02:
            d.category = core::AuditCategory::Configuration;
            break;
        case 0x03:
            d.category = core::AuditCategory::Access;
            break;
        case 0x04:
            d.category = core::AuditCategory::RiskControl;
            break;
        case 0x05:
            d.category = core::AuditCategory::Operator;
            break;
        default:
            return result;
    }
    switch (pha) {
        case 0x01:
            d.phase = core::AuditPhase::Requested;
            break;
        case 0x02:
            d.phase = core::AuditPhase::Confirmed;
            break;
        case 0x03:
            d.phase = core::AuditPhase::Executed;
            break;
        case 0x04:
            d.phase = core::AuditPhase::Failed;
            break;
        default:
            return result;
    }
    if (timeP == 1)
        d.time = core::RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{static_cast<std::int64_t>(timeValue)}});
    if (srcP == 1)
        d.sourceSequence = srcValue;
    d.detailTruncated = trunc == 1;
    result.record     = d;
    result.status     = CanonicalReadStatus::Ok;
    return result;
}

}  // namespace mddlog::adapter
