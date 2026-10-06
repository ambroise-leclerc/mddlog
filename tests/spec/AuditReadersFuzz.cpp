/** @brief libFuzzer entry point; all allocations are bounded by a 64 KiB input and four segments. */
import std;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

#include "../framework/AuditReaderExercise.hpp"

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer requires this C ABI entry point.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size <= 65536)
        mddlog::spec::exerciseReaderInput({data, size});
    return 0;
}
