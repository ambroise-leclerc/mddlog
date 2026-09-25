/**
 * @brief mddlog::core::InlineString<N>: capacity boundaries, exact vs. truncating assignment, the
 *        UTF-8 truncation boundary at 2/3/4-byte sequences, already-malformed input, and value
 *        comparison (ADR-001 Decisions 1-2, issue #33).
 *
 * InlineString<N> lives in mddlog-core (mddlog.core.inlinestring), imported directly here rather
 * than through the mddlog umbrella module: it is not re-exported there, being a governed-zone-only
 * building block at this stage (ADR-001 Decision 1's full governed record does not exist yet).
 *
 * N out of the std::uint16_t length's representable range is rejected by the class template's
 * `requires` clause at compile time (see InlineString.cppm) rather than exercised here: this
 * project has no compile-fail test harness yet (tracked separately, not introduced by #33), so
 * that criterion is satisfied by the `requires` clause itself plus this note, per issue #33's
 * documented fallback.
 */
import std;
import speclab;
import mddlog.core.inlinestring;

namespace {

using mddlog::core::InlineString;

// Octal escapes, not hex: a \x escape's value must fit the (possibly signed) char range or is
// diagnosed as out-of-range by some compilers under -Werror, while an octal escape's value is
// reduced mod 256 without a portability warning - and every byte below is >= 0x80.
//
// U+00E9 (é): 0xC3 0xA9 = octal 303 251 - 2 bytes.
constexpr std::string_view twoByteChar = "\303\251";
// U+20AC (€): 0xE2 0x82 0xAC = octal 342 202 254 - 3 bytes.
constexpr std::string_view threeByteChar = "\342\202\254";
// U+1F600 (😀): 0xF0 0x9F 0x98 0x80 = octal 360 237 230 200 - 4 bytes.
constexpr std::string_view fourByteChar = "\360\237\230\200";

const speclab::Register isTriviallyCopyableAndConstexprConstructible{
    "InlineString<N> is trivially copyable and default-constructs to empty",
    "unit",
    [] {
        return speclab::Test("inlinestring-trivially-copyable")
            .Then("std::is_trivially_copyable_v holds, and a default-constructed instance is empty",
                  [] {
                      speclab::core::Checks checks;
                      checks.expect(std::is_trivially_copyable_v<InlineString<16>>, "InlineString<16> is trivially copyable");
                      InlineString<16> s;
                      checks.expect(s.empty(), "default-constructed instance is empty");
                      checks.expect(s.size() == 0, "default-constructed instance has size 0");
                      checks.expect(s.view().empty(), "default-constructed instance's view() is empty");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register assignExactAcceptsUpToExactCapacity{
    "assignExact() stores a value that fits whole, including exactly at capacity",
    "unit",
    [] {
        return speclab::Test("inlinestring-assign-exact-fits")
            .Then("an empty value, a partial value, and a value exactly at capacity are all accepted and stored whole",
                  [] {
                      speclab::core::Checks checks;

                      InlineString<8> empty;
                      checks.expect(empty.assignExact(""), "assignExact(\"\") succeeds");
                      checks.expect(empty.view() == "", "empty value stored as empty");

                      InlineString<8> partial;
                      checks.expect(partial.assignExact("hi"), "assignExact(\"hi\") succeeds under capacity 8");
                      checks.expect(partial.view() == "hi", "partial value stored exactly");

                      InlineString<8> exact;
                      checks.expect(exact.assignExact("exactly8"), "assignExact() succeeds at exactly capacity 8");
                      checks.expect(exact.view() == "exactly8", "capacity-length value stored exactly");
                      checks.expect(exact.size() == 8, "size() reports the full 8 bytes");

                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register assignExactRejectsOverflowWithoutModifyingTheValue{
    "assignExact() refuses a value one byte over capacity, and leaves the previous value untouched",
    "unit",
    [] {
        return speclab::Test("inlinestring-assign-exact-overflow-unchanged")
            .Then("a capacity+1 value is refused, and a prior value already held is preserved intact",
                  [] {
                      speclab::core::Checks checks;
                      InlineString<8>       s;
                      checks.expect(s.assignExact("original"), "seeding the value with a capacity-length string succeeds");
                      checks.expect(!s.assignExact("overflow!"), "assignExact() refuses a 9-byte value against capacity 8");
                      checks.expect(s.view() == "original", "the refused assignment left the previous value exactly as it was");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register assignTruncatingKeepsWholeValueWhenItFits{
    "assignTruncating() reports no truncation when the value fits whole",
    "unit",
    [] {
        return speclab::Test("inlinestring-assign-truncating-fits")
            .Then("an empty value and a value exactly at capacity are stored whole, with truncated == false",
                  [] {
                      speclab::core::Checks checks;

                      InlineString<8> empty;
                      checks.expect(!empty.assignTruncating(""), "assignTruncating(\"\") reports no truncation");
                      checks.expect(empty.empty(), "empty value stays empty");

                      InlineString<8> exact;
                      checks.expect(!exact.assignTruncating("exactly8"), "an exactly-capacity value reports no truncation");
                      checks.expect(exact.view() == "exactly8", "the value is stored whole");

                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register assignTruncatingNeverSplitsAWellFormedUtf8SequenceAtTheBoundary{
    "assignTruncating() never splits a well-formed UTF-8 sequence at the capacity boundary, for 2, 3 and 4-byte sequences",
    "unit",
    [] {
        return speclab::Test("inlinestring-assign-truncating-utf8-boundary")
            .Then("a multi-byte character that fits whole is kept; one that would be cut is dropped in its entirety, "
                  "never leaving a partial lead or continuation byte, for each of 2, 3 and 4-byte sequences",
                  [] {
                      speclab::core::Checks checks;

                      // 2-byte: "a" + é. Capacity 3 fits the whole thing; capacity 2 cannot fit any of é.
                      {
                          InlineString<3> fits;
                          const bool      truncated = fits.assignTruncating(std::string("a") + std::string(twoByteChar));
                          checks.expect(!truncated, "2-byte char: exact-fit capacity reports no truncation");
                          checks.expect(fits.view() == std::string("a") + std::string(twoByteChar), "2-byte char: kept whole at exact-fit capacity");

                          InlineString<2> cut;
                          const bool      droppedTruncated = cut.assignTruncating(std::string("a") + std::string(twoByteChar));
                          checks.expect(droppedTruncated, "2-byte char: undersized capacity reports truncation");
                          checks.expect(cut.view() == "a", "2-byte char: the whole 2-byte sequence is dropped, only \"a\" remains");
                      }

                      // 3-byte: "a" + €. Capacity 4 fits the whole thing; capacity 3 cannot fit any of €.
                      {
                          InlineString<4> fits;
                          const bool      truncated = fits.assignTruncating(std::string("a") + std::string(threeByteChar));
                          checks.expect(!truncated, "3-byte char: exact-fit capacity reports no truncation");
                          checks.expect(fits.view() == std::string("a") + std::string(threeByteChar), "3-byte char: kept whole at exact-fit capacity");

                          InlineString<3> cut;
                          const bool      droppedTruncated = cut.assignTruncating(std::string("a") + std::string(threeByteChar));
                          checks.expect(droppedTruncated, "3-byte char: undersized capacity reports truncation");
                          checks.expect(cut.view() == "a", "3-byte char: the whole 3-byte sequence is dropped, only \"a\" remains");
                      }

                      // 4-byte: "a" + 😀. Capacity 5 fits the whole thing; capacity 4 cannot fit any of 😀.
                      {
                          InlineString<5> fits;
                          const bool      truncated = fits.assignTruncating(std::string("a") + std::string(fourByteChar));
                          checks.expect(!truncated, "4-byte char: exact-fit capacity reports no truncation");
                          checks.expect(fits.view() == std::string("a") + std::string(fourByteChar), "4-byte char: kept whole at exact-fit capacity");

                          InlineString<4> cut;
                          const bool      droppedTruncated = cut.assignTruncating(std::string("a") + std::string(fourByteChar));
                          checks.expect(droppedTruncated, "4-byte char: undersized capacity reports truncation");
                          checks.expect(cut.view() == "a", "4-byte char: the whole 4-byte sequence is dropped, only \"a\" remains");
                      }

                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register assignTruncatingKeepsACompleteSequenceThatEndsExactlyAtTheBoundary{
    "assignTruncating() keeps a complete multi-byte sequence that ends exactly at the capacity boundary, even when more "
    "input follows it - the case a backward-only search that never restores the original boundary would get wrong",
    "unit",
    [] {
        return speclab::Test("inlinestring-assign-truncating-utf8-boundary-exact-then-more")
            .Then("\"a\" + <char> + \"x\", with capacity sized to hold exactly \"a\" + <char>, keeps the full character "
                  "and drops only the trailing \"x\", for each of 2, 3 and 4-byte sequences",
                  [] {
                      speclab::core::Checks checks;

                      // 2-byte: "a" + é + "x", capacity 3 (= 1 + 2) fits "a" + é whole and drops "x".
                      {
                          InlineString<3> s;
                          const bool      truncated = s.assignTruncating(std::string("a") + std::string(twoByteChar) + "x");
                          checks.expect(truncated, "2-byte char followed by more input: truncation is reported");
                          checks.expect(s.view() == std::string("a") + std::string(twoByteChar), "2-byte char: kept whole, trailing \"x\" dropped");
                      }

                      // 3-byte: "a" + € + "x", capacity 4 (= 1 + 3) fits "a" + € whole and drops "x".
                      {
                          InlineString<4> s;
                          const bool      truncated = s.assignTruncating(std::string("a") + std::string(threeByteChar) + "x");
                          checks.expect(truncated, "3-byte char followed by more input: truncation is reported");
                          checks.expect(s.view() == std::string("a") + std::string(threeByteChar), "3-byte char: kept whole, trailing \"x\" dropped");
                      }

                      // 4-byte: "a" + 😀 + "x", capacity 5 (= 1 + 4) fits "a" + 😀 whole and drops "x".
                      {
                          InlineString<5> s;
                          const bool      truncated = s.assignTruncating(std::string("a") + std::string(fourByteChar) + "x");
                          checks.expect(truncated, "4-byte char followed by more input: truncation is reported");
                          checks.expect(s.view() == std::string("a") + std::string(fourByteChar), "4-byte char: kept whole, trailing \"x\" dropped");
                      }

                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register assignTruncatingHandlesAlreadyMalformedInputDeterministically{
    "assignTruncating() on already-malformed UTF-8 input stops at the documented 3-byte backtrack cap, without crashing",
    "unit",
    [] {
        return speclab::Test("inlinestring-assign-truncating-malformed-input")
            .Then("6 consecutive stray continuation bytes against a 5-byte capacity are cut at capacity-3, "
                  "the documented behavior when the backtrack cap is hit without finding a lead byte",
                  [] {
                      speclab::core::Checks checks;
                      const std::string     strayContinuationBytes(6, '\200');  // 0x80, octal to avoid an out-of-range hex escape warning
                      InlineString<5>       s;
                      const bool            truncated = s.assignTruncating(strayContinuationBytes);
                      checks.expect(truncated, "malformed input over capacity reports truncation");
                      checks.expect(s.size() == 5 - 3, "the cut lands at capacity - 3, per the documented backtrack cap");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register equalityComparesLogicalContentNotRawStorage{
    "InlineString equality compares logical content (view()), not raw storage bytes left behind by a shorter later write",
    "unit",
    [] {
        return speclab::Test("inlinestring-equality-logical-content")
            .Then("two instances holding the same short value after different write histories compare equal, "
                  "and an instance compares equal to an equivalent std::string_view",
                  [] {
                      speclab::core::Checks checks;

                      InlineString<8> direct;
                      checks.expect(direct.assignExact("hi"), "direct assignment of the short value succeeds");

                      InlineString<8> overwritten;
                      checks.expect(overwritten.assignExact("original"), "seed with a longer, capacity-length value");
                      checks.expect(overwritten.assignExact("hi"), "overwrite with the same short value as 'direct'");

                      checks.expect(direct == overwritten, "both instances compare equal despite different write histories");
                      checks.expect(direct == std::string_view("hi"), "an instance compares equal to an equivalent std::string_view");

                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
