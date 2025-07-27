/**
 * @brief Base sink interface for medical device logging - C++23 Module
 */

export module mddlog.sinks.sink;

import std;
import mddlog.core.loglevel;
import mddlog.core.logrecord;

export namespace mddlog::sinks {

    /**
     * @brief Abstract base class for all log sinks
     * 
     * Sinks are responsible for outputting log records to various destinations
     * such as files, console, network, or audit systems. Each sink can have
     * its own formatting and filtering logic.
     */
    class Sink {
    public:
        /**
         * @brief Virtual destructor for proper cleanup
         */
        virtual ~Sink() = default;

        /**
         * @brief Write a log record to the sink
         * @param record The log record to write
         */
        virtual void write(const core::LogRecord& record) = 0;

        /**
         * @brief Flush any buffered log records
         */
        virtual void flush() = 0;

        /**
         * @brief Check if this sink should log the given level
         * @param level Log level to check
         * @return True if the level should be logged
         */
        virtual bool shouldLog(core::LogLevel level) const noexcept {
            return level >= minLevel_;
        }

        /**
         * @brief Set the minimum log level for this sink
         * @param level Minimum level to log
         */
        void setMinLevel(core::LogLevel level) noexcept {
            minLevel_ = level;
        }

        /**
         * @brief Get the minimum log level
         * @return Current minimum level
         */
        core::LogLevel getMinLevel() const noexcept {
            return minLevel_;
        }

        /**
         * @brief Get the sink name
         * @return Sink identifier
         */
        virtual std::string_view getName() const noexcept = 0;

        /**
         * @brief Check if the sink is enabled
         * @return True if sink is active
         */
        bool isEnabled() const noexcept {
            return enabled_;
        }

        /**
         * @brief Enable or disable the sink
         * @param enabled New enabled state
         */
        void setEnabled(bool enabled) noexcept {
            enabled_ = enabled;
        }

        /**
         * @brief Get sink statistics
         * @return Reference to statistics object
         */
        virtual const core::LogStatistics& getStatistics() const noexcept {
            return statistics_;
        }

    protected:
        /**
         * @brief Protected constructor for derived classes
         * @param minLevel Initial minimum log level
         */
        explicit Sink(core::LogLevel minLevel = core::LogLevel::INFO)
            : minLevel_(minLevel), enabled_(true) {
        }

        /**
         * @brief Update statistics for a write operation
         * @param bytesWritten Number of bytes written
         * @param writeTime Time taken to write
         */
        void updateStatistics(std::size_t bytesWritten, std::chrono::nanoseconds writeTime) noexcept {
            statistics_.recordsWritten.fetch_add(1);
            statistics_.bytesWritten.fetch_add(bytesWritten);
            statistics_.totalWriteTimeNs.fetch_add(static_cast<std::uint64_t>(writeTime.count()));
        }

        /**
         * @brief Record a dropped record in statistics
         */
        void recordDropped() noexcept {
            statistics_.recordsDropped.fetch_add(1);
        }

        /**
         * @brief Record a flush operation in statistics
         */
        void recordFlush() noexcept {
            statistics_.flushCount.fetch_add(1);
        }

    private:
        std::atomic<core::LogLevel> minLevel_;      ///< Minimum log level
        std::atomic<bool> enabled_;                 ///< Sink enabled state
        mutable core::LogStatistics statistics_;   ///< Performance statistics
    };

    /**
     * @brief Shared pointer type for sinks
     */
    using SinkPtr = std::shared_ptr<Sink>;

    /**
     * @brief Weak pointer type for sinks
     */
    using WeakSinkPtr = std::weak_ptr<Sink>;

} // namespace mddlog::sinks