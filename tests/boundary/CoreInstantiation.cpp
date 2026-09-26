/** @brief Emit representative governed template bodies for the separate object scans. */
import std;
import mddlog.core.ring;

template class mddlog::core::RingLog<1>;
template class mddlog::core::RingLog<3>;
template class mddlog::core::InlineString<mddlog::core::messageCapacity>;

// NOLINTNEXTLINE(misc-use-internal-linkage): external linkage keeps this scan probe emitted in optimized builds.
[[nodiscard]] mddlog::core::WriteResult assignGoverned(mddlog::core::GovernedRecord& record, const mddlog::core::RecordInput& input) noexcept {
    return record.assign(input);
}
