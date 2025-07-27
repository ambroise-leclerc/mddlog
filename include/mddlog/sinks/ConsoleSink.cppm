/**
 * @brief Console sink for medical device logging - C++23 Module
 */

export module mddlog.sinks.console;

import std;
import mddlog.core.loglevel;
import mddlog.core.logrecord;
import mddlog.sinks.sink;

export namespace mddlog::sinks {

    /**
     * @brief Console sink that outputs log records to stdout/stderr
     * 
     * This sink provides colored output for different log levels and
     * is thread-safe for concurrent logging operations.
     */
    class ConsoleSink : public Sink {
    public:
        /**
         * @brief Constructor
         * @param useColors Enable colored output (default: true)
         * @param useStderr Use stderr for ERROR and FATAL levels (default: true)
         */
        explicit ConsoleSink(bool useColors = true, bool useStderr = true)
            : Sink(core::LogLevel::INFO)
            , useColors_(useColors)
            , useStderr_(useStderr) {
        }

        /**
         * @brief Destructor - ensures final flush
         */
        ~ConsoleSink() override {
            flush();
        }

        /**
         * @brief Write a log record to console
         * @param record The log record to write
         */
        void write(const core::LogRecord& record) override {
            if (!shouldLog(record.level) || !isEnabled()) {
                recordDropped();
                return;
            }

            auto start = std::chrono::high_resolution_clock::now();
            
            std::lock_guard<std::mutex> lock(mutex_);
            
            try {
                // Choose output stream based on log level
                std::ostream& stream = (useStderr_ && 
                    (record.level >= core::LogLevel::ERROR)) ? std::cerr : std::cout;

                // Add color if enabled
                if (useColors_) {
                    stream << core::getColorCode(record.level);
                }

                // Format: [TIMESTAMP] [LEVEL] [CATEGORY] MESSAGE
                stream << "[" << record.getFormattedTimestamp() << "] "
                       << "[" << core::toString(record.level) << "] "
                       << "[" << record.category << "] "
                       << record.message;

                // Add medical device context if available
                if (!record.userId.empty()) {
                    stream << " [User:" << record.userId << "]";
                }
                if (!record.deviceId.empty()) {
                    stream << " [Device:" << record.deviceId << "]";
                }
                if (!record.operationId.empty()) {
                    stream << " [Op:" << record.operationId << "]";
                }

                // Add audit information if present
                if (!record.auditEventType.empty()) {
                    stream << " [Audit:" << record.auditEventType << "]";
                }
                if (!record.riskLevel.empty()) {
                    stream << " [Risk:" << record.riskLevel << "]";
                }

                // Add thread information in debug builds
                #ifdef MDDLOG_DEBUG
                stream << " [Thread:" << record.threadId << "]";
                #endif

                // Reset color if enabled
                if (useColors_) {
                    stream << core::getResetColorCode();
                }

                stream << "\n";

                // Update statistics
                auto end = std::chrono::high_resolution_clock::now();
                auto writeTime = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                
                // Estimate bytes written (rough approximation)
                std::size_t bytesWritten = record.message.length() + record.category.length() + 50;
                updateStatistics(bytesWritten, writeTime);

            } catch (const std::exception&) {
                // Record failure but don't throw - logging should not crash the application
                recordDropped();
            }
        }

        /**
         * @brief Flush the console output
         */
        void flush() override {
            std::lock_guard<std::mutex> lock(mutex_);
            std::cout.flush();
            std::cerr.flush();
            recordFlush();
        }

        /**
         * @brief Get the sink name
         * @return "console"
         */
        std::string_view getName() const noexcept override {
            return "console";
        }

        /**
         * @brief Check if colors are enabled
         * @return True if colored output is enabled
         */
        bool isColorEnabled() const noexcept {
            return useColors_;
        }

        /**
         * @brief Enable or disable colored output
         * @param enabled New color state
         */
        void setColorEnabled(bool enabled) noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            useColors_ = enabled;
        }

        /**
         * @brief Check if stderr is used for errors
         * @return True if stderr is used for ERROR and FATAL levels
         */
        bool isStderrEnabled() const noexcept {
            return useStderr_;
        }

        /**
         * @brief Enable or disable stderr for error levels
         * @param enabled New stderr state
         */
        void setStderrEnabled(bool enabled) noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            useStderr_ = enabled;
        }

    private:
        mutable std::mutex mutex_;          ///< Thread synchronization
        bool useColors_;                    ///< Enable colored output
        bool useStderr_;                    ///< Use stderr for errors
    };

    /**
     * @brief Factory function to create a console sink
     * @param useColors Enable colored output
     * @param useStderr Use stderr for error levels
     * @return Shared pointer to console sink
     */
    SinkPtr createConsoleSink(bool useColors = true, bool useStderr = true) {
        return std::make_shared<ConsoleSink>(useColors, useStderr);
    }

} // namespace mddlog::sinks