/**
 * @brief Basic usage example for MddLog medical device logger
 */

import std;
import mddlog;

using namespace mddlog;

int main() {  // NOLINT(bugprone-exception-escape): example code; an uncaught exception terminating the demo is acceptable
    std::cout << "MddLog Basic Usage Example\n";
    std::cout << "==========================\n\n";

    // Create a logger instance
    auto logger = std::make_unique<SimpleLogger>("MedicalDevice", true);

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

    std::cout << "\n3. Audit trail logging:\n";

    // Audit trail logging
    logger->logAudit("User accessed patient data",
                     "DATA_ACCESS",  // Event type
                     "user123",      // User ID
                     "device_789",   // Device ID
                     "LOW"           // Risk level
    );

    logger->logAudit("System configuration changed", "CONFIG_CHANGE", "admin456", "device_789", "MEDIUM");

    logger->logAudit("Emergency shutdown initiated", "EMERGENCY_SHUTDOWN", "system", "device_789", "HIGH");

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