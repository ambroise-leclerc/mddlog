/**
 * @brief Fixed-capacity, owned, allocation-free string storage - C++23 Module
 */

export module mddlog.core.inlinestring;

import std;

export namespace mddlog::core {

namespace detail {

// UTF-8 lead-byte patterns: (byte & mask) == tag identifies the byte's role. A continuation byte
// is 10xxxxxx; a lead byte's high bits count how many continuation bytes follow it.
inline constexpr unsigned char continuationByteMask = 0xC0;
inline constexpr unsigned char continuationByteTag  = 0x80;
inline constexpr unsigned char asciiByteMask        = 0x80;
inline constexpr unsigned char asciiByteTag         = 0x00;
inline constexpr unsigned char twoByteLeadMask      = 0xE0;
inline constexpr unsigned char twoByteLeadTag       = 0xC0;
inline constexpr unsigned char threeByteLeadMask    = 0xF0;
inline constexpr unsigned char threeByteLeadTag     = 0xE0;
inline constexpr unsigned char fourByteLeadMask     = 0xF8;
inline constexpr unsigned char fourByteLeadTag      = 0xF0;

// The longest UTF-8 sequence is 4 bytes (1 lead + 3 continuation), so a continuation byte is at
// most 3 positions past its sequence's lead byte - the bound the truncation-boundary search below
// backtracks by.
inline constexpr std::size_t maxUtf8ContinuationBytes = 3;

constexpr bool isUtf8ContinuationByte(char c) noexcept {
    return (static_cast<unsigned char>(c) & continuationByteMask) == continuationByteTag;
}

/// Expected total length of the UTF-8 sequence starting with lead byte `c`. Returns 1 for ASCII,
/// for a continuation byte (never a valid lead), and for an invalid lead byte (0xF8-0xFF): the
/// governed core does not validate well-formedness, so those cases are treated as an opaque
/// single byte rather than rejected.
constexpr std::size_t utf8SequenceLength(char c) noexcept {
    const auto b = static_cast<unsigned char>(c);
    if ((b & asciiByteMask) == asciiByteTag)
        return 1;
    if ((b & twoByteLeadMask) == twoByteLeadTag)
        return 2;
    if ((b & threeByteLeadMask) == threeByteLeadTag)
        return 3;
    if ((b & fourByteLeadMask) == fourByteLeadTag)
        return 4;
    return 1;
}

/**
 * @brief Longest prefix of `value` that fits in `capacity` bytes without ending in the middle of
 *        a well-formed UTF-8 sequence (ADR-001 Decision 2).
 *
 * Walks backward from the byte-`capacity` boundary, capped at maxUtf8ContinuationBytes bytes of
 * backtracking, to find the lead byte of the sequence straddling the cut point. If that sequence
 * fits whole within `capacity` after all - it was simply sitting right at the boundary - the
 * original boundary stands unchanged; if it does not fit, the whole sequence is dropped rather
 * than left partial. For input that is already malformed before this ever runs -
 * maxUtf8ContinuationBytes or more consecutive continuation bytes ending at the cut point, which
 * cannot occur in well-formed UTF-8 within this bound - the search stops unconditionally at the
 * cap and the cut is made there, whatever it lands on; this function never validates or repairs
 * malformed input, it only avoids introducing a fresh split in input that was well-formed to
 * begin with.
 */
constexpr std::size_t utf8TruncationBoundary(std::string_view value, std::size_t capacity) noexcept {
    if (value.size() <= capacity)
        return value.size();

    // Scans backward from `capacity`, looking for the first non-continuation byte at or before
    // it - the lead byte of the sequence (if any) straddling the cut point.
    std::size_t pos   = capacity;
    std::size_t steps = 0;
    while (steps < maxUtf8ContinuationBytes && pos > 0 && isUtf8ContinuationByte(value[pos - 1])) {
        --pos;
        ++steps;
    }

    if (pos == 0 || isUtf8ContinuationByte(value[pos - 1])) {
        // The backtrack cap was hit without reaching a non-continuation byte - only reachable
        // with already-malformed input - and `pos` stands as the cut, whatever it lands on.
        return pos;
    }

    const std::size_t leadPos  = pos - 1;
    const std::size_t expected = utf8SequenceLength(value[leadPos]);
    if (leadPos + expected <= capacity) {
        // The sequence starting at leadPos fits whole within capacity after all - it was simply
        // sitting right at the boundary - so the original boundary needs no adjustment.
        return capacity;
    }
    return leadPos;  // the sequence does not fit whole within capacity; drop it entirely
}

}  // namespace detail

/**
 * @brief Fixed-capacity, owned, allocation-free string storage (ADR-001 Decisions 1-2).
 *
 * A std::array<char, N> plus a std::uint16_t length: constexpr, noexcept, trivially copyable, no
 * allocation and no exception anywhere in this type. `N` is bounded to what the std::uint16_t
 * length field can represent.
 */
template <std::size_t N>
    requires(N <= std::numeric_limits<std::uint16_t>::max())
class InlineString {
public:
    static constexpr std::size_t capacity = N;

    constexpr InlineString() noexcept = default;

    /**
     * @brief Exact assignment, for identifier-kind fields (ADR-001 Decision 2).
     * @return true and stores `value` if it fits whole; false and leaves this object exactly as
     *         it was before the call otherwise - no partial identifier is ever stored.
     */
    constexpr bool assignExact(std::string_view value) noexcept {
        if (value.size() > N)
            return false;
        std::copy_n(value.begin(), value.size(), bytes.begin());
        length = static_cast<std::uint16_t>(value.size());
        return true;
    }

    /**
     * @brief Truncating assignment, for descriptive-text fields (ADR-001 Decision 2).
     * @return true if `value` had to be shortened to fit; false if it fit whole. Always stores
     *         the (possibly shortened) result; never fails.
     */
    constexpr bool assignTruncating(std::string_view value) noexcept {
        const std::size_t copyLen = detail::utf8TruncationBoundary(value, N);
        std::copy_n(value.begin(), copyLen, bytes.begin());
        length = static_cast<std::uint16_t>(copyLen);
        return copyLen < value.size();
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(bytes.data(), length);
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return length;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return length == 0;
    }

    [[nodiscard]] friend constexpr bool operator==(const InlineString& lhs, const InlineString& rhs) noexcept {
        return lhs.view() == rhs.view();
    }

    [[nodiscard]] friend constexpr bool operator==(const InlineString& lhs, std::string_view rhs) noexcept {
        return lhs.view() == rhs;
    }

private:
    std::array<char, N> bytes{};
    std::uint16_t       length = 0;
};

namespace detail::selftest {

// Compile-time proof (ADR-001 issue #33's constexpr-evaluation criterion): every operation below
// runs entirely at compile time, so a regression that introduces a non-constexpr-friendly
// construct (an allocation, a throw, anything the constant-evaluator rejects) fails the build
// here rather than surfacing only in a runtime spec.

consteval bool exactAssignStoresValueThatFitsWhole() {
    constexpr std::string_view     value = "hello";
    InlineString<value.size() + 1> s;  // one byte of headroom above the exact length
    return s.assignExact(value) && s.view() == value && s.size() == value.size();
}
static_assert(exactAssignStoresValueThatFitsWhole());

consteval bool exactAssignRejectsOverflowWithoutModifyingTheValue() {
    constexpr std::string_view     value = "hello";
    InlineString<value.size() - 1> s;  // one byte short of what `value` needs
    const bool                     accepted = s.assignExact(value);
    return !accepted && s.empty();
}
static_assert(exactAssignRejectsOverflowWithoutModifyingTheValue());

consteval bool truncatingAssignNeverSplitsAWellFormedTwoByteSequence() {
    // U+00E9 (é) encodes as octal \303\251 (hex 0xC3 0xA9) - two bytes. Octal, not hex: a \x
    // escape's value must fit char's range or some compilers diagnose it as out-of-range, while
    // an octal escape's value is reduced mod 256 without that warning. A one-byte capacity cannot
    // hold any of it without splitting the sequence, so the whole character is dropped rather
    // than storing a lone lead byte.
    InlineString<1> s;
    const bool      truncated = s.assignTruncating("\303\251");
    return truncated && s.empty();
}
static_assert(truncatingAssignNeverSplitsAWellFormedTwoByteSequence());

consteval bool truncatingAssignKeepsACompleteSequenceThatEndsExactlyAtCapacityEvenWithMoreInputFollowing() {
    // "a" + é ("\303\251") + "x" is 4 bytes; capacity 3 ends exactly after "a" + é. A regression
    // that never restores the boundary once it confirms the straddling sequence fits whole would
    // cut back into the middle of é instead (this pins the bug a review caught before merge).
    InlineString<3> s;
    const bool      truncated = s.assignTruncating("a\303\251x");
    return truncated && s.view() == "a\303\251";
}
static_assert(truncatingAssignKeepsACompleteSequenceThatEndsExactlyAtCapacityEvenWithMoreInputFollowing());

}  // namespace detail::selftest

}  // namespace mddlog::core
