/**
 * @brief Fixed-capacity governed log record and host-supplied time (ADR-001 Decisions 1-3, 7).
 */

export module mddlog.core.record;

import std;
export import mddlog.core.inlinestring;
export import mddlog.core.loglevel;
export import mddlog.core.writeresult;

export namespace mddlog::core {

inline constexpr std::size_t messageCapacity            = 160;
inline constexpr std::size_t componentCapacity          = 32;
inline constexpr std::size_t operationIdCapacity        = 32;
inline constexpr std::size_t correlationIdCapacity      = 40;
inline constexpr std::size_t maximumStandardRecordBytes = 384;

/** @brief Whether the host supplied a usable Unix-time value. */
enum class TimeAvailability : std::uint8_t { Available, Unavailable };

/**
 * @brief Host-supplied UTC time, or an explicit unavailable state.
 *
 * Only the two factories can construct this type. Neither reads a clock or validates the
 * plausibility of an available timestamp; no value can produce MalformedTime under ADR-001
 * Decision 7. An unavailable timestamp is admitted and rendered later by the adapter.
 */
class RawTime {
public:
    /** @brief Wrap nanoseconds since the Unix epoch supplied by the host. */
    [[nodiscard]] static constexpr RawTime available(std::chrono::sys_time<std::chrono::nanoseconds> value) noexcept {
        return {value, TimeAvailability::Available};
    }

    /** @brief Represent a host without a usable clock. */
    [[nodiscard]] static constexpr RawTime unavailable() noexcept {
        return {{}, TimeAvailability::Unavailable};
    }

    [[nodiscard]] constexpr TimeAvailability availability() const noexcept {
        return tag;
    }

    /** @brief Return the supplied time; meaningful only when availability() is Available. */
    [[nodiscard]] constexpr std::chrono::sys_time<std::chrono::nanoseconds> value() const noexcept {
        return timestamp;
    }

private:
    constexpr RawTime(std::chrono::sys_time<std::chrono::nanoseconds> initialValue, TimeAvailability initialTag) noexcept
        : timestamp(initialValue), tag(initialTag) {}

    std::chrono::sys_time<std::chrono::nanoseconds> timestamp;
    TimeAvailability                                tag;
};

/**
 * @brief Caller-owned data captured when the producer emits a record.
 *
 * String views are read only during assign(); the resulting record owns their bytes. The default
 * location captures the aggregate-initialization site, and the default level is Info. The host
 * must supply time explicitly.
 */
struct RecordInput {
    LogLevel             level = LogLevel::Info;
    RawTime              time;
    std::source_location location = std::source_location::current();
    std::string_view     message;
    std::string_view     component;
    std::string_view     operationId;
    std::string_view     correlationId;
};

/**
 * @brief Owned, fixed-capacity record for the governed SPSC ring.
 *
 * This name distinguishes the governed value from the existing allocating
 * mddlog::core::LogRecord in the adapter module, which issue #35 leaves unchanged. A failed
 * assign() leaves every byte of the previous record intact. A default-constructed value is an
 * empty, unpublished slot with unavailable time.
 */
class GovernedRecord {
public:
    constexpr GovernedRecord() noexcept = default;

    /**
     * @brief Capture the supplied context and message, or refuse an overlong identifier.
     *
     * Identifiers are checked in declaration order: component, operationId, correlationId.
     * Only after all fit is the message copied and, if needed, shortened at a UTF-8 boundary.
     * A refusal does not change this record. MalformedTime is unreachable for RawTime; ring
     * capacity is checked by RingLog, not by this record.
     *
     * @param input Values and source location supplied at the emission site.
     * @return Written with the message-truncation flag, or IdentifierTooLong with its field.
     */
    [[nodiscard]] constexpr WriteResult assign(const RecordInput& input) noexcept {
        GovernedRecord next;
        if (!next.componentValue.assignExact(input.component))
            return WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::Component});
        if (!next.operationIdValue.assignExact(input.operationId))
            return WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::OperationId});
        if (!next.correlationIdValue.assignExact(input.correlationId))
            return WriteResult::refused({.reason = RefusalReason::IdentifierTooLong, .field = IdentifierField::CorrelationId});

        next.levelValue             = input.level;
        next.timeValue              = input.time;
        next.locationValue          = input.location;
        next.truncatedValue.message = next.messageValue.assignTruncating(input.message);
        *this                       = next;
        return WriteResult::written(truncatedValue);
    }

    [[nodiscard]] constexpr LogLevel level() const noexcept {
        return levelValue;
    }

    [[nodiscard]] constexpr RawTime time() const noexcept {
        return timeValue;
    }

    [[nodiscard]] constexpr std::source_location location() const noexcept {
        return locationValue;
    }

    [[nodiscard]] constexpr std::string_view message() const noexcept {
        return messageValue.view();
    }

    [[nodiscard]] constexpr std::string_view component() const noexcept {
        return componentValue.view();
    }

    [[nodiscard]] constexpr std::string_view operationId() const noexcept {
        return operationIdValue.view();
    }

    [[nodiscard]] constexpr std::string_view correlationId() const noexcept {
        return correlationIdValue.view();
    }

    [[nodiscard]] constexpr TruncatedFields truncated() const noexcept {
        return truncatedValue;
    }

private:
    LogLevel                            levelValue = LogLevel::Info;
    RawTime                             timeValue  = RawTime::unavailable();
    std::source_location                locationValue;
    InlineString<messageCapacity>       messageValue;
    InlineString<componentCapacity>     componentValue;
    InlineString<operationIdCapacity>   operationIdValue;
    InlineString<correlationIdCapacity> correlationIdValue;
    TruncatedFields                     truncatedValue;
};

static_assert(std::is_trivially_copyable_v<RawTime>);
static_assert(std::is_trivially_copyable_v<GovernedRecord>);
// ADR-001 Decision 8 estimates approximately 320 bytes on a 64-bit target. Allow ABI padding
// while rejecting an accidental unbounded or substantially larger representation.
static_assert(sizeof(GovernedRecord) <= maximumStandardRecordBytes);

}  // namespace mddlog::core
