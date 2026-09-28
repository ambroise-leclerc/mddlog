/**
 * @brief Simple usage example demonstrating the static Log helper
 */

import std;
import mddlog;
import mddlog.log;

using namespace mddlog;

int main() {  // NOLINT(bugprone-exception-escape): example code; an uncaught exception terminating the demo is acceptable
    std::cout << "Simple Log Helper Example\n";
    std::cout << "=========================\n\n";

    // Initialize with custom settings (optional - auto-initializes if not called)
    Log::initialize("MedicalApp", true, true);

    // Set minimum log level
    Log::setMinLevel(LogLevel::Debug);

    std::cout << "1. Basic logging with static Log helper:\n";

    // Simple logging - no boilerplate!
    Log::trace("Starting system initialization");
    Log::debug("Loading configuration files");
    Log::info("Medical device system ready");
    Log::warn("Temperature sensor reading elevated");
    Log::error("Failed to connect to monitoring server");
    Log::fatal("Critical system failure detected");

    std::cout << "\n2. Medical compliance logging:\n";

    // Medical compliance logging
    Log::logMedical(LogLevel::Info,
                    "Patient monitoring session started",
                    "patient_monitor",
                    "user123",      // User ID
                    "session_456",  // Session ID
                    "device_789"    // Device ID
    );

    AuditRing<1> auditRing{"device_789:boot_demo:simple"};
    Log::setAuditRing(auditRing);
    const auto audit = Log::logAudit({.category      = AuditCategory::Configuration,
                                      .phase         = AuditPhase::Executed,
                                      .action        = "CONFIG_CHANGE",
                                      .actor         = "admin456",
                                      .target        = "device_789",
                                      .correlationId = "device_789:boot_demo:ui:1",
                                      .detail        = "Configuration changed by administrator"});
    if (!audit.wasAdmitted())
        return 1;

    std::cout << "\n3. Multi-threaded logging test:\n";

    // Test thread safety
    std::vector<std::thread> threads;
    threads.reserve(3);
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([i]() {
            for (int j = 0; j < 3; ++j) {
                Log::info("Thread " + std::to_string(i) + " message " + std::to_string(j), "threading");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    std::cout << "\n4. Category-based logging:\n";

    // Different categories
    Log::info("Network connection established", "network");
    Log::debug("Processing sensor data", "sensors");
    Log::warn("Battery level low", "hardware");
    Log::error("Database connection lost", "database");

    // Flush and cleanup
    Log::flush();
    Log::clearAuditRing();

    std::cout << "\nSimple logging example completed!\n";
    return 0;
}
