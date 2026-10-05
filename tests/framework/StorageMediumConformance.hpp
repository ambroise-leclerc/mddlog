/** @brief Common observable StorageMedium invariants, independent of deployment durability. */
#ifndef MDDLOG_TESTS_FRAMEWORK_STORAGEMEDIUMCONFORMANCE_HPP
#define MDDLOG_TESTS_FRAMEWORK_STORAGEMEDIUMCONFORMANCE_HPP

import std;
import mddlog.adapter.auditmedium;
import speclab;

namespace mddlog::tests {

inline void checkStorageMediumConformance(adapter::StorageMedium& medium, speclab::core::Checks& checks) {
    using namespace adapter;
    const std::array<std::uint8_t, 4> opening{1, 2, 3, 4};
    const std::array<std::uint8_t, 3> addition{5, 6, 7};
    const auto                        created = medium.open({.streamId = "opaque/stream", .bytes = opening});
    checks.expect(created.status == OpenStatus::Opened && created.segment != 0 && created.end == opening.size(), "opening returns opaque identity and end");
    const auto appended = medium.append(created.segment, addition);
    checks.expect(appended.status == AppendStatus::Written && appended.end == opening.size() + addition.size(), "append extends without replacing bytes");
    const auto read = medium.read(created.segment, 2, 100);
    checks.expect(read == std::optional(std::vector<std::uint8_t>{3, 4, 5, 6, 7}), "read returns available range");
    checks.expect(medium.read(created.segment, 7, 10) == std::optional(std::vector<std::uint8_t>{}), "EOF is an empty successful read");
    checks.expect(medium.read(created.segment, 0, 0) == std::optional(std::vector<std::uint8_t>{}), "zero length succeeds");
    const auto inventory = medium.segments();
    checks.expect(inventory && inventory->size() == 1 && inventory->front().segment == created.segment && inventory->front().size == appended.end,
                  "inventory contains the written extent");
    const auto other = medium.open({.streamId = "opaque/stream", .bytes = opening});
    checks.expect(other.status == OpenStatus::Opened && other.segment != created.segment, "identities distinguish even equal logical openings");
    checks.expect(medium.reclaim(created.segment), "reclaim removes whole segment");
    checks.expect(!medium.read(created.segment, 0, 4), "unavailable differs from EOF");
    const auto remaining = medium.segments();
    checks.expect(remaining && remaining->size() == 1 && remaining->front().segment == other.segment, "reclaim preserves the other segment");
}

}  // namespace mddlog::tests

#endif  // MDDLOG_TESTS_FRAMEWORK_STORAGEMEDIUMCONFORMANCE_HPP
