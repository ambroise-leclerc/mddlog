/** @brief Ledger records: building them, their typed view and the validation of ADR-004 10.2. Adapter zone only. */

export module mddlog.adapter.auditledger;

import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditchain;

export namespace mddlog::adapter {

/** @brief The seven kinds of ledger record, one per reserved action (10.2). */
enum class LedgerRecordKind : std::uint8_t {
    /** @brief Record 1: the adapter found no earlier ledger in the log. */
    Origin,
    /** @brief Record 1: the adapter found an earlier ledger and cites its last position that checks. */
    Predecessor,
    /** @brief One earlier stream the log still holds, cited at its last position that checks. */
    Recovered,
    /** @brief The adapter accepted a stream; written before any record of that stream is stored. */
    StreamOpen,
    /** @brief Orderly close of a stream. */
    StreamClose,
    /** @brief Records `1 … q` of a stream are about to be removed. */
    StreamTrim,
    /** @brief Orderly end of the session. */
    LedgerClose
};

namespace ledgeraction {
inline constexpr std::string_view origin      = "mddlog.ledger.origin";
inline constexpr std::string_view predecessor = "mddlog.ledger.predecessor";
inline constexpr std::string_view recovered   = "mddlog.stream.recovered";
inline constexpr std::string_view streamOpen  = "mddlog.stream.open";
inline constexpr std::string_view streamClose = "mddlog.stream.close";
inline constexpr std::string_view streamTrim  = "mddlog.stream.trim";
inline constexpr std::string_view ledgerClose = "mddlog.ledger.close";
}  // namespace ledgeraction

[[nodiscard]] constexpr std::string_view ledgerActionName(LedgerRecordKind kind) noexcept {
    switch (kind) {
        case LedgerRecordKind::Origin:
            return ledgeraction::origin;
        case LedgerRecordKind::Predecessor:
            return ledgeraction::predecessor;
        case LedgerRecordKind::Recovered:
            return ledgeraction::recovered;
        case LedgerRecordKind::StreamOpen:
            return ledgeraction::streamOpen;
        case LedgerRecordKind::StreamClose:
            return ledgeraction::streamClose;
        case LedgerRecordKind::StreamTrim:
            return ledgeraction::streamTrim;
        case LedgerRecordKind::LedgerClose:
            return ledgeraction::ledgerClose;
    }
    return {};
}

/** @brief The kind an action names, by exact match; empty for an ordinary action and for a reserved action the table does not list (10.2). */
[[nodiscard]] constexpr std::optional<LedgerRecordKind> ledgerKindOf(std::string_view action) noexcept {
    for (const auto kind : {LedgerRecordKind::Origin,
                            LedgerRecordKind::Predecessor,
                            LedgerRecordKind::Recovered,
                            LedgerRecordKind::StreamOpen,
                            LedgerRecordKind::StreamClose,
                            LedgerRecordKind::StreamTrim,
                            LedgerRecordKind::LedgerClose}) {
        if (ledgerActionName(kind) == action)
            return kind;
    }
    return std::nullopt;
}

/** @brief A record 1 that makes its stream a ledger (10.1). */
[[nodiscard]] constexpr bool isLedgerHeadAction(std::string_view action) noexcept {
    return action == ledgeraction::origin || action == ledgeraction::predecessor;
}

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-constant-array-index):
// hexadecimal digit extraction over a fixed 32-byte digest.
/** @brief Exactly 64 lowercase hexadecimal characters, the only textual form of a digest (8.4). Empty for anything else. */
[[nodiscard]] constexpr std::optional<Sha256Digest> digestFromHex(std::string_view text) noexcept {
    if (text.size() != 2 * sha256DigestSize)
        return std::nullopt;
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        return -1;
    };
    Sha256Digest digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = nibble(text[2 * i]);
        const int low  = nibble(text[(2 * i) + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        digest[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return digest;
}
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers,hicpp-signed-bitwise,cppcoreguidelines-pro-bounds-constant-array-index)

/** @brief What the adapter means to write as one ledger record: the typed side of the table in 10.2. */
struct LedgerEntry {
    LedgerRecordKind kind  = LedgerRecordKind::StreamOpen;
    core::AuditPhase phase = core::AuditPhase::Executed;
    /** @brief The stream the record is about; the ledger's own identity for `origin` and `ledger.close`. */
    std::string                  target;
    std::optional<std::uint64_t> sourceSequence;
    /** @brief Written as 64 lowercase hexadecimal characters; empty when absent. */
    std::optional<Sha256Digest> digest;
    /** @brief Free text for people, never interpreted by a reader. */
    std::string detail;

    [[nodiscard]] static LedgerEntry make(LedgerRecordKind kind, std::string_view target) {
        LedgerEntry entry;
        entry.kind   = kind;
        entry.target = std::string{target};
        return entry;
    }
    [[nodiscard]] static LedgerEntry
    cite(LedgerRecordKind kind, std::string_view target, bool checks, std::optional<std::uint64_t> position, std::optional<Sha256Digest> digest) {
        LedgerEntry entry    = make(kind, target);
        entry.phase          = checks ? core::AuditPhase::Executed : core::AuditPhase::Failed;
        entry.sourceSequence = position;
        entry.digest         = digest;
        return entry;
    }
    [[nodiscard]] static LedgerEntry position(LedgerRecordKind kind, std::string_view target, std::uint64_t at, const Sha256Digest& digestAt) {
        LedgerEntry entry    = make(kind, target);
        entry.sourceSequence = at;
        entry.digest         = digestAt;
        return entry;
    }

    [[nodiscard]] static LedgerEntry origin(std::string_view ledgerId) {
        return make(LedgerRecordKind::Origin, ledgerId);
    }
    /** @brief `Executed` when the earlier ledger checks. `Failed` cites the last position that checks, or nothing when none does. */
    [[nodiscard]] static LedgerEntry
    predecessor(std::string_view earlier, bool checks, std::optional<std::uint64_t> position, std::optional<Sha256Digest> digest) {
        return cite(LedgerRecordKind::Predecessor, earlier, checks, position, digest);
    }
    [[nodiscard]] static LedgerEntry
    recovered(std::string_view stream, bool checks, std::optional<std::uint64_t> position, std::optional<Sha256Digest> digest) {
        return cite(LedgerRecordKind::Recovered, stream, checks, position, digest);
    }
    [[nodiscard]] static LedgerEntry streamOpen(std::string_view stream) {
        return make(LedgerRecordKind::StreamOpen, stream);
    }
    [[nodiscard]] static LedgerEntry streamClose(std::string_view stream, std::uint64_t position, const Sha256Digest& digest) {
        return LedgerEntry::position(LedgerRecordKind::StreamClose, stream, position, digest);
    }
    [[nodiscard]] static LedgerEntry streamTrim(std::string_view stream, std::uint64_t position, const Sha256Digest& digest) {
        return LedgerEntry::position(LedgerRecordKind::StreamTrim, stream, position, digest);
    }
    [[nodiscard]] static LedgerEntry ledgerClose(std::string_view ledgerId) {
        return make(LedgerRecordKind::LedgerClose, ledgerId);
    }
};

/**
 * @brief Build the audit event of a ledger record. This is the path the adapter owns: AuditEvent::assign() does not refuse the reserved prefix, only
 * producer admission does (10.1). Empty when the sequence is 0 or an identity is not valid.
 */
[[nodiscard]] inline std::optional<core::AuditEvent>
buildLedgerEvent(const LedgerEntry& entry, std::string_view ledgerStreamId, std::uint64_t sequence, core::RawTime time) {
    std::array<char, 2 * sha256DigestSize> hex{};
    std::string_view                       correlation;
    if (entry.digest.has_value()) {
        hex         = digestToHex(*entry.digest);
        correlation = std::string_view{hex.data(), hex.size()};
    }
    core::AuditInput input;
    input.category       = core::AuditCategory::Lifecycle;
    input.phase          = entry.phase;
    input.time           = time;
    input.action         = ledgerActionName(entry.kind);
    input.target         = entry.target;
    input.correlationId  = correlation;
    input.sourceSequence = entry.sourceSequence;
    input.detail         = entry.detail;
    core::AuditEvent event;
    if (!event.assign(input, ledgerStreamId, sequence).wasAdmitted())
        return std::nullopt;
    return event;
}

/** @brief A ledger record as a reader sees it, its fields named by their ledger meaning (10.2). The views point into the decoded record. */
struct LedgerRecordView {
    LedgerRecordKind             kind  = LedgerRecordKind::StreamOpen;
    core::AuditPhase             phase = core::AuditPhase::Executed;
    std::string_view             target;
    std::optional<std::uint64_t> sourceSequence;
    /** @brief The digest of `correlationId`; empty when that field is empty. */
    std::optional<Sha256Digest> digest;
};

/** @brief Why a ledger record is malformed: each rule of 10.2, "Validating ledger records". */
enum class LedgerFault : std::uint8_t {
    /** @brief An action under `mddlog.` that the table does not list. */
    UnknownReservedAction,
    /** @brief An ordinary action: only ledger records belong in a ledger. */
    NotALedgerAction,
    WrongCategory,
    WrongPhase,
    /** @brief `actor`, `requirementRef` or `riskRef` is not empty. */
    FieldNotEmpty,
    /** @brief `sourceSequence` present where the table says absent, or absent where it requires a position. */
    SourceSequenceRule,
    /** @brief `correlationId` not empty where the table says empty, or not exactly 64 lowercase hexadecimal characters where it carries a digest. */
    CorrelationRule,
    /** @brief `origin` or `predecessor` after record 1, or record 1 that is neither. */
    HeadMisplaced,
    /** @brief The `target` of `origin` or `ledger.close` is not the ledger's own identity. */
    TargetNotOwn,
    /** @brief A `recovered` record after a record other than record 1 or another `recovered`. */
    RecoveredMisplaced,
    RecordAfterLedgerClose,
    /** @brief `close` twice for one stream. */
    CloseTwice,
    /** @brief A record other than `trim` after the stream's `close`. */
    RecordAfterClose,
    /** @brief The stream's trims do not have increasing positions. */
    TrimNotIncreasing,
    /** @brief A trim's position exceeds its stream's `close` position. */
    TrimBeyondClose
};

struct LedgerCheck {
    /** @brief Set when the record is malformed: the operation it describes is never applied (10.2). */
    std::optional<LedgerFault> fault;
    /** @brief Set when the record is a valid ledger record. */
    std::optional<LedgerRecordView> view;
};

/**
 * @brief Checks the records of one ledger stream, in sequence order, against the table of 10.2.
 *
 * The caller has already verified the chain. A malformed record changes no state, so what it describes is never applied: an invalid trim accounts for no
 * missing records and an invalid close ends no stream.
 */
class LedgerChecker {
public:
    explicit LedgerChecker(std::string_view ledgerStreamId) : own(ledgerStreamId) {}

    [[nodiscard]] LedgerCheck check(const DecodedAuditRecord& record) {
        const std::uint64_t position = ++seen;
        const auto          found    = ledgerKindOf(record.action);
        const auto          previous = std::exchange(previousKind, found);
        if (!found.has_value()) {
            const LedgerFault fault = core::isReservedAuditAction(record.action) ? LedgerFault::UnknownReservedAction : LedgerFault::NotALedgerAction;
            return {.fault = fault, .view = std::nullopt};
        }
        const LedgerRecordKind kind = *found;
        if (const auto fault = shape(record, kind, position, previous); fault.has_value())
            return {.fault = fault, .view = std::nullopt};
        // shape() accepted the fields, so the correlation is empty or a digest.
        LedgerRecordView view;
        view.kind           = kind;
        view.phase          = record.phase;
        view.target         = record.target;
        view.sourceSequence = record.sourceSequence;
        if (!record.correlationId.empty())
            view.digest = digestFromHex(record.correlationId);
        if (const auto fault = order(view); fault.has_value())
            return {.fault = fault, .view = std::nullopt};
        apply(view);
        return {.fault = std::nullopt, .view = view};
    }

private:
    struct StreamState {
        bool                         closed        = false;
        std::uint64_t                closePosition = 0;
        std::optional<std::uint64_t> lastTrim;
    };

    [[nodiscard]] std::optional<LedgerFault>
    shape(const DecodedAuditRecord& record, LedgerRecordKind kind, std::uint64_t position, const std::optional<LedgerRecordKind>& previous) const {
        if (record.category != core::AuditCategory::Lifecycle)
            return LedgerFault::WrongCategory;
        const bool mayFail = kind == LedgerRecordKind::Predecessor || kind == LedgerRecordKind::Recovered;
        const bool phaseOk = record.phase == core::AuditPhase::Executed || (mayFail && record.phase == core::AuditPhase::Failed);
        if (!phaseOk)
            return LedgerFault::WrongPhase;
        if (!record.actor.empty() || !record.requirementRef.empty() || !record.riskRef.empty())
            return LedgerFault::FieldNotEmpty;
        const bool headKind = kind == LedgerRecordKind::Origin || kind == LedgerRecordKind::Predecessor;
        if (headKind != (position == 1))
            return LedgerFault::HeadMisplaced;
        if (ledgerClosed)
            return LedgerFault::RecordAfterLedgerClose;
        // `recovered` may follow record 1 or another `recovered`, and nothing else.
        if (kind == LedgerRecordKind::Recovered && position != 2 && previous != LedgerRecordKind::Recovered)
            return LedgerFault::RecoveredMisplaced;
        const bool digestKind = kind == LedgerRecordKind::StreamClose || kind == LedgerRecordKind::StreamTrim;
        const bool citing     = kind == LedgerRecordKind::Predecessor || kind == LedgerRecordKind::Recovered;
        if (digestKind || (citing && record.phase == core::AuditPhase::Executed)) {
            if (!record.sourceSequence.has_value())
                return LedgerFault::SourceSequenceRule;
            if (!digestFromHex(record.correlationId).has_value())
                return LedgerFault::CorrelationRule;
        } else if (citing) {  // the Failed form cites the last position that checks, or nothing at all
            if (record.sourceSequence.has_value() != !record.correlationId.empty())
                return record.sourceSequence.has_value() ? LedgerFault::CorrelationRule : LedgerFault::SourceSequenceRule;
            if (record.sourceSequence.has_value() && !digestFromHex(record.correlationId).has_value())
                return LedgerFault::CorrelationRule;
        } else {
            if (record.sourceSequence.has_value())
                return LedgerFault::SourceSequenceRule;
            if (!record.correlationId.empty())
                return LedgerFault::CorrelationRule;
        }
        if ((kind == LedgerRecordKind::Origin || kind == LedgerRecordKind::LedgerClose) && record.target != own)
            return LedgerFault::TargetNotOwn;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<LedgerFault> order(const LedgerRecordView& view) const {
        const std::uint64_t at = view.sourceSequence.value_or(0);
        if (view.kind == LedgerRecordKind::StreamClose || view.kind == LedgerRecordKind::StreamTrim || view.kind == LedgerRecordKind::StreamOpen
            || view.kind == LedgerRecordKind::Recovered) {
            const auto found = streams.find(std::string{view.target});
            if (found == streams.end())
                return std::nullopt;
            const StreamState& state = found->second;
            if (view.kind == LedgerRecordKind::StreamClose && state.closed)
                return LedgerFault::CloseTwice;
            if (state.closed && view.kind != LedgerRecordKind::StreamTrim)
                return LedgerFault::RecordAfterClose;
            if (view.kind == LedgerRecordKind::StreamTrim) {
                if (state.lastTrim.has_value() && at <= state.lastTrim.value_or(0))
                    return LedgerFault::TrimNotIncreasing;
                if (state.closed && at > state.closePosition)
                    return LedgerFault::TrimBeyondClose;
            }
        }
        return std::nullopt;
    }

    void apply(const LedgerRecordView& view) {
        switch (view.kind) {
            case LedgerRecordKind::StreamClose: {
                StreamState& state  = streams[std::string{view.target}];
                state.closed        = true;
                state.closePosition = view.sourceSequence.value_or(0);
                break;
            }
            case LedgerRecordKind::StreamTrim:
                streams[std::string{view.target}].lastTrim = view.sourceSequence;
                break;
            case LedgerRecordKind::StreamOpen:
                (void)streams[std::string{view.target}];
                break;
            case LedgerRecordKind::LedgerClose:
                ledgerClosed = true;
                break;
            default:
                break;
        }
    }

    std::string                                     own;
    std::uint64_t                                   seen         = 0;
    bool                                            ledgerClosed = false;
    std::optional<LedgerRecordKind>                 previousKind;
    std::map<std::string, StreamState, std::less<>> streams;
};

}  // namespace mddlog::adapter
