/** @brief Hash chain per stream instance over canonical records (ADR-004 8.4). Adapter zone only. */

export module mddlog.adapter.auditchain;

import std;
export import mddlog.adapter.sha256;
export import mddlog.adapter.auditcanonical;
import mddlog.core.auditevent;

export namespace mddlog::adapter {

/** @brief H_0: 32 bytes of 0x00, the same for every stream (8.4). */
inline constexpr Sha256Digest chainInitialValue{};

/** @brief H_k = SHA-256(C_k || H_{k-1}); the previous digest enters as its 32 raw bytes, last. */
[[nodiscard]] constexpr Sha256Digest chainDigest(std::span<const std::uint8_t> canonicalBytes, const Sha256Digest& previous) noexcept {
    Sha256 hasher;
    hasher.update(canonicalBytes);
    hasher.update(previous);
    return hasher.finish();
}

/** @brief A digest as 64 lowercase hexadecimal characters, the only textual form (8.4). */
[[nodiscard]] constexpr std::array<char, 2 * sha256DigestSize> digestToHex(const Sha256Digest& digest) noexcept {
    constexpr std::string_view             digits = "0123456789abcdef";
    std::array<char, 2 * sha256DigestSize> text{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        text[2 * i]     = digits[digest[i] >> 4];
        text[2 * i + 1] = digits[digest[i] & 0x0F];
    }
    return text;
}

/** @brief Why the writer refused to chain an event; the chain state is unchanged. */
enum class ChainRefusal : std::uint8_t {
    /** @brief The event belongs to another stream instance. */
    WrongStream,
    /** @brief The event's sequence is not the next one of this instance (8.4, chain order). */
    OutOfOrder,
    /** @brief An enumerator has no byte in the contract tables. */
    NotEncodable
};

/** @brief One chained record: its canonical bytes C_k and its digest H_k. */
struct ChainedRecord {
    CanonicalRecord canonical;
    Sha256Digest    digest{};
};

/**
 * @brief Writer-side chain of one stream instance.
 *
 * A restart begins a new chain with a new stream identity (ADR-002 Decision 5); the link between
 * instances is a ledger record (ADR-004 Decision 10), never a different H_0. Performs no I/O,
 * holds no key and is not thread-safe: the stream's single drain owns it.
 */
class AuditChain {
public:
    /** @brief Valid only for a stream identity AuditEvent accepts; check valid() before use. */
    explicit constexpr AuditChain(std::string_view streamId) noexcept : validStream(core::AuditEvent::validStreamId(streamId)) {
        if (validStream)
            (void)stream.assignExact(streamId);
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return validStream;
    }

    /** @brief Encode and chain the next event. Advances the chain only on success. */
    [[nodiscard]] constexpr std::expected<ChainedRecord, ChainRefusal> append(const core::AuditEvent& event) noexcept {
        if (!validStream || event.streamId() != stream.view())
            return std::unexpected{ChainRefusal::WrongStream};
        if (event.sequence() != position + 1)
            return std::unexpected{ChainRefusal::OutOfOrder};
        auto canonical = CanonicalRecord::encode(event);
        if (!canonical.has_value())
            return std::unexpected{ChainRefusal::NotEncodable};
        ChainedRecord out{.canonical = *canonical, .digest = chainDigest(canonical->bytes(), head)};
        head     = out.digest;
        position = event.sequence();
        return out;
    }

    /** @brief Sequence of the last chained record; 0 before the first. */
    [[nodiscard]] constexpr std::uint64_t chainedThrough() const noexcept {
        return position;
    }
    /** @brief H at chainedThrough(); H_0 before the first record. */
    [[nodiscard]] constexpr const Sha256Digest& headDigest() const noexcept {
        return head;
    }

private:
    bool                                          validStream;
    core::InlineString<core::auditStreamCapacity> stream;
    std::uint64_t                                 position = 0;
    Sha256Digest                                  head     = chainInitialValue;
};

/** @brief What a reader found at one stored record (ADR-004 8.3 and 8.4, ahead of the 7.5 verdicts). */
enum class ChainFinding : std::uint8_t {
    Ok,
    /** @brief Malformed: short, version 0, or a field rule failed. Reported as Inconsistent. */
    Malformed,
    /** @brief Version differs from the stream's. Reported as Inconsistent, known version or not. */
    VersionChange,
    /** @brief The stream's version is not implemented here; nothing was decoded. Cannot verify. */
    UnsupportedVersion,
    /** @brief streamId differs from the instance's. Inconsistent. */
    StreamMismatch,
    /** @brief sequence is not the next one. Inconsistent. */
    SequenceBreak,
    /** @brief Stored digest differs from the recomputed one. Inconsistent. */
    DigestMismatch
};

/**
 * @brief Reader-side check of stored records of one stream instance, in storage order.
 *
 * It hashes the bytes as stored, never a re-encoding of the decoded fields (8.4). Stops advancing
 * at the first finding other than Ok: later calls keep returning the first failure's position
 * through failedAt(), and no further record is accepted.
 */
class AuditChainVerifier {
public:
    /** @brief Start a stream instance at H_0, expecting sequence 1. */
    explicit constexpr AuditChainVerifier(std::string_view streamId) noexcept : AuditChainVerifier(streamId, 0, chainInitialValue) {}

    /** @brief Start after a recorded position, as a trimmed prefix does (8.4, 10.4). */
    constexpr AuditChainVerifier(std::string_view streamId, std::uint64_t afterSequence, const Sha256Digest& afterDigest) noexcept
        : position(afterSequence), head(afterDigest) {
        (void)stream.assignExact(streamId);
    }

    /** @brief Check the next stored record against its stored digest, in 8.3's order. */
    [[nodiscard]] constexpr ChainFinding check(std::span<const std::uint8_t> bytes, const Sha256Digest& storedDigest) noexcept {
        if (failed)
            return firstFinding;
        const ChainFinding finding = evaluate(bytes, storedDigest);
        if (finding != ChainFinding::Ok) {
            failed       = true;
            firstFinding = finding;
            failedSeq    = position + 1;
        }
        return finding;
    }

    /** @brief Sequence of the last record that verified; 0 or the starting point before any. */
    [[nodiscard]] constexpr std::uint64_t verifiedThrough() const noexcept {
        return position;
    }
    [[nodiscard]] constexpr const Sha256Digest& headDigest() const noexcept {
        return head;
    }
    /** @brief The expected sequence of the first failing record, once a finding other than Ok occurred. */
    [[nodiscard]] constexpr std::optional<std::uint64_t> failedAt() const noexcept {
        return failed ? std::optional<std::uint64_t>{failedSeq} : std::nullopt;
    }

private:
    [[nodiscard]] constexpr ChainFinding evaluate(std::span<const std::uint8_t> bytes, const Sha256Digest& storedDigest) noexcept {
        // 8.3 steps 1 and 2 need no knowledge of any version, so they precede an unknown version.
        const std::uint16_t version = canonicalVersionOf(bytes);
        if (version == 0)
            return ChainFinding::Malformed;
        if (streamVersion == 0)
            streamVersion = version;
        else if (version != streamVersion)
            return ChainFinding::VersionChange;
        if (version != canonicalContractVersion)
            return ChainFinding::UnsupportedVersion;

        const CanonicalReadResult read = decodeCanonical(bytes);
        if (read.status != CanonicalReadStatus::Ok)
            return ChainFinding::Malformed;
        if (read.record.streamId != stream.view())
            return ChainFinding::StreamMismatch;
        if (read.record.sequence != position + 1)
            return ChainFinding::SequenceBreak;
        const Sha256Digest expected = chainDigest(bytes, head);
        if (expected != storedDigest)
            return ChainFinding::DigestMismatch;
        head     = expected;
        position = read.record.sequence;
        return ChainFinding::Ok;
    }

    core::InlineString<core::auditStreamCapacity> stream;
    std::uint64_t                                 position;
    Sha256Digest                                  head;
    std::uint16_t                                 streamVersion = 0;
    bool                                          failed        = false;
    ChainFinding                                  firstFinding  = ChainFinding::Ok;
    std::uint64_t                                 failedSeq     = 0;
};

}  // namespace mddlog::adapter
