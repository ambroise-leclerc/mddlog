/**
 * @brief Allocation-free admission result for governed writes (ADR-001 Decision 3).
 */

export module mddlog.core.writeresult;

import std;

export namespace mddlog::core {

/** @brief Outcome of an attempted governed write. */
enum class Admission : std::uint8_t { Written, Refused };

/** @brief Why a governed write was refused. */
enum class RefusalReason : std::uint8_t { RingFull, IdentifierTooLong, MalformedTime };

/** @brief Identifier fields in the order checked for overlong values. */
enum class IdentifierField : std::uint8_t { Component, OperationId, CorrelationId };

/**
 * @brief First reason a governed write was refused.
 *
 * `field` is meaningful only for IdentifierTooLong. The write path checks MalformedTime first,
 * then identifiers in declaration order (Component, OperationId, CorrelationId), and RingFull
 * last. The first failure wins. Under the current RawTime contract, MalformedTime is unreachable:
 * Unavailable time is admitted (ADR-001 Decision 7).
 */
struct Refusal {
    RefusalReason   reason;
    IdentifierField field;
};

/** @brief Descriptive fields shortened in a successfully written record. */
struct TruncatedFields {
    bool message = false;
};

/**
 * @brief Admission and truncation of one governed write, returned by value.
 *
 * `refusal` is meaningful only when admission is Refused; `truncated` is meaningful only when
 * admission is Written. A refused write has no stored record and therefore no truncated fields.
 * Use the factories to construct those states. No exception, allocation, or text conversion is
 * involved. ADR-001 Decision 3 deliberately keeps this small result instead of std::expected;
 * a broader governed API would require a separate decision about adopting std::expected.
 */
struct [[nodiscard]] WriteResult {
    Admission       admission;
    Refusal         refusal;
    TruncatedFields truncated;

    /** @brief Report an admitted write and its descriptive-field truncation. */
    [[nodiscard]] static constexpr WriteResult written(TruncatedFields shortened = {}) noexcept {
        return {
            Admission::Written,
            {.reason = RefusalReason::RingFull, .field = IdentifierField::Component},
            shortened
        };
    }

    /** @brief Report a refused write; no truncation is reported. */
    [[nodiscard]] static constexpr WriteResult refused(Refusal failure) noexcept {
        return {Admission::Refused, failure, {}};
    }

private:
    constexpr WriteResult(Admission outcome, Refusal failure, TruncatedFields shortened) noexcept
        : admission(outcome), refusal(failure), truncated(shortened) {}
};

static_assert(std::is_trivially_copyable_v<WriteResult>);

}  // namespace mddlog::core
