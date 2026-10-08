/** @brief Use the full package through the umbrella and the Log facade only (#121). */
import std;
import mddlog;
import mddlog.log;

int main() {
    // A real assertion, not just "it links": the module rebuilt by this consumer reports the
    // version the package declares, so the version definitions reached the consumer's BMI.
    if (mddlog::getVersion() != MDDLOG_CONSUMER_EXPECTED_VERSION)
        return 1;
    if (!mddlog::isMedicalComplianceEnabled())
        return 2;

    mddlog::SimpleLogger logger("package-consumer", false);
    logger.addSink(mddlog::createConsoleSink(false, false));
    if (logger.tryLog(mddlog::LogLevel::Info, "package consumer smoke test") != mddlog::DiagnosticStatus::Success)
        return 3;
    if (logger.flushChecked().status != mddlog::DiagnosticStatus::Success)
        return 4;

    // The governed ring reached through the umbrella and bridged by the adapter target.
    mddlog::RingLog<1>      ring;
    mddlog::RingSinkAdapter adapter;
    adapter.addRing(ring);
    if (ring.tryWrite({.time = mddlog::RawTime::unavailable(), .message = "installed bridge"}).admission() != mddlog::Admission::Written
        || adapter.drainOnce() != 1)
        return 5;

    // The global facade: explicit initialization and a checked shutdown.
    mddlog::Log::initializeWithConfig("package-consumer-global", mddlog::DiagnosticConfig{.messageCapacity = 4}, false, false);
    mddlog::Log::info("facade reachable");
    if (mddlog::Log::shutdownChecked().status != mddlog::DiagnosticStatus::Success)
        return 6;
    return 0;
}
