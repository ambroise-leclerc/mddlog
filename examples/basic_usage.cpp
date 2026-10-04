/**
 * @brief Basic usage example for MddLog medical device logger
 */

import std;
import mddlog;

using namespace mddlog;

int main() {  // NOLINT(bugprone-exception-escape): example code; an uncaught exception terminating the demo is acceptable
    std::cout << "MddLog Basic Usage Example\n";
    std::cout << "==========================\n\n";

    AuditRing<4> auditRing{"device_789:boot_demo:basic"};
    // Create a logger instance
    auto logger = std::make_unique<SimpleLogger>("MedicalDevice", true);
    logger->setAuditRing(auditRing);

    // Add a console sink with colors
    auto consoleSink = createConsoleSink(true, true);
    logger->addSink(consoleSink);

    // Set minimum log level
    logger->setMinLevel(LogLevel::Debug);

    std::cout << "1. Basic logging examples:\n";

    // Basic logging examples
    logger->trace("System initialization starting");
    logger->debug("Loading configuration files");
    logger->info("Medical device system started successfully");
    logger->warn("Temperature sensor reading is slightly elevated");
    logger->error("Failed to connect to external monitoring system");
    logger->fatal("Critical system failure detected");

    std::cout << "\n2. Medical compliance logging:\n";

    // Medical compliance logging
    logger->logMedical(LogLevel::Info,
                       "Patient monitoring session started",
                       "patient_monitor",
                       "user123",      // User ID
                       "session_456",  // Session ID
                       "device_789"    // Device ID
    );

    logger->logMedical(LogLevel::Warn, "Heart rate threshold exceeded", "vital_signs", "user123", "session_456", "device_789");

    std::cout << "\n3. In-memory audit admission:\n";

    const auto access        = logger->logAudit({.category      = AuditCategory::Access,
                                                 .phase         = AuditPhase::Executed,
                                                 .action        = "DATA_ACCESS",
                                                 .actor         = "user123",
                                                 .target        = "device_789",
                                                 .correlationId = "device_789:boot_demo:ui:1",
                                                 .detail        = "User accessed patient data"});
    const auto configuration = logger->logAudit({.category      = AuditCategory::Configuration,
                                                 .phase         = AuditPhase::Executed,
                                                 .action        = "CONFIG_CHANGE",
                                                 .actor         = "admin456",
                                                 .target        = "device_789",
                                                 .correlationId = "device_789:boot_demo:ui:2",
                                                 .detail        = "System configuration changed"});
    const auto shutdown      = logger->logAudit({.category      = AuditCategory::RiskControl,
                                                 .phase         = AuditPhase::Requested,
                                                 .action        = "EMERGENCY_SHUTDOWN",
                                                 .actor         = "system",
                                                 .target        = "device_789",
                                                 .correlationId = "device_789:boot_demo:ui:3",
                                                 .detail        = "Emergency shutdown requested"});
    if (!access.wasAdmitted() || !configuration.wasAdmitted() || !shutdown.wasAdmitted())
        return 1;
    // Admission ends at memory. A real host must configure an AuditSinkAdapter and inspect its health.

    std::cout << "\n4. Multi-threaded logging test:\n";

    // Multi-threaded logging test
    std::vector<std::thread> threads;
    threads.reserve(3);

    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&logger, i]() {
            for (int j = 0; j < 5; ++j) {
                logger->info("Thread " + std::to_string(i) + " message " + std::to_string(j), "thread_test");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    std::cout << "\n5. Performance test:\n";

    // Performance test
    const int numMessages = 1000;
    auto      start       = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numMessages; ++i) {
        logger->debug("Performance test message " + std::to_string(i), "perf_test");
    }

    // Ensure all messages are processed
    logger->flush();
    logger->clearAuditRing();

    auto end      = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Logged " << numMessages << " messages in " << duration.count() << " microseconds\n";
    std::cout << "Average: " << (duration.count() / numMessages) << " microseconds per message\n";

    std::cout << "\n6. Sink statistics:\n";

    // Display sink statistics
    const auto& stats = consoleSink->getStatistics();
    std::cout << "Console Sink Statistics:\n";
    std::cout << "  Records written: " << stats.recordsWritten.load() << "\n";
    std::cout << "  Records dropped: " << stats.recordsDropped.load() << "\n";
    std::cout << "  Bytes written: " << stats.bytesWritten.load() << "\n";
    std::cout << "  Flush count: " << stats.flushCount.load() << "\n";
    std::cout << "  Total write time: " << stats.getTotalWriteTime().count() << " ns\n";

    std::cout << "\n7. Logger configuration:\n";

    // Display logger configuration
    std::cout << "Logger name: " << logger->getName() << "\n";
    std::cout << "Async logging: " << (logger->isAsyncLogging() ? "enabled" : "disabled") << "\n";
    std::cout << "Minimum level: " << mddlog::toString(logger->getMinLevel()) << "\n";
    std::cout << "Sink count: " << logger->getSinkCount() << "\n";
    std::cout << "Logger enabled: " << (logger->isEnabled() ? "yes" : "no") << "\n";

    // Final flush and cleanup
    logger->flush();

    std::cout << "\nExample completed successfully!\n";
    return 0;
}
