/** @brief Centralized diagnostic configuration and monitoring; business emissions stay short. */
import std;
import mddlog;
import mddlog.log;

namespace {
// This business function has the same call before and after bounded configuration.
void runOperation() {
    mddlog::Log::info("Operation started", "pump");
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): example initialization errors terminate the process.
int main() {
    mddlog::Log::initializeWithConfig("pump", {.messageCapacity = 64, .flushCapacity = 2, .maxRecordBytes = 256, .sinkCapacity = 4});
    runOperation();
    const auto result = mddlog::Log::flushFor(std::chrono::milliseconds(100));
    const auto health = mddlog::Log::health();
    if (health) {
        std::cout << "admitted=" << health->admitted << " refused=" << health->saturated + health->oversized + health->stopped + health->reentrant
                  << " write_failures=" << health->writeFailures << " flush_failures=" << health->flushFailures
                  << " internal_failures=" << health->internalFailures << '\n';
    }
    mddlog::Log::shutdown();
    return result.status == mddlog::DiagnosticStatus::Success ? 0 : 1;
}
