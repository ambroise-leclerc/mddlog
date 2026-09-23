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
        return level >= minLevel.load();
    }

    /**
     * @brief Set the minimum log level for this sink
     * @param level Minimum level to log
     */
    void setMinLevel(core::LogLevel level) noexcept {
        minLevel.store(level);
    }

    /**
     * @brief Get the minimum log level
     * @return Current minimum level
     */
    core::LogLevel getMinLevel() const noexcept {
        return minLevel.load();
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
        return enabled.load();
    }

    /**
     * @brief Enable or disable the sink
     * @param value New enabled state
     */
    void setEnabled(bool value) noexcept {
        enabled.store(value);
    }

    /**
     * @brief Get sink statistics
     * @return Reference to statistics object
     */
    virtual const core::LogStatistics& getStatistics() const noexcept {
        return statistics;
    }

    /**
     * @brief Record that a write to this sink threw and was suppressed by the caller
     *
     * Callers outside the sink hierarchy (the logger's dispatch loop) cannot reach the
     * protected statistics helpers below, so this is the explicit, observable record of a
     * caught write failure: it shows up in getStatistics().recordsDropped rather than
     * disappearing into a silent catch(...).
     */
    void recordWriteFailure() noexcept {
        statistics.recordsDropped.fetch_add(1);
    }

protected:
    /**
     * @brief Protected constructor for derived classes
     * @param initialMinLevel Initial minimum log level
     */
    explicit Sink(core::LogLevel initialMinLevel = core::LogLevel::INFO) : minLevel(initialMinLevel), enabled(true) {}

    /**
     * @brief Update statistics for a write operation
     * @param bytesWritten Number of bytes written
     * @param writeTime Time taken to write
     */
    void updateStatistics(std::size_t bytesWritten, std::chrono::nanoseconds writeTime) noexcept {
        statistics.recordsWritten.fetch_add(1);
        statistics.bytesWritten.fetch_add(bytesWritten);
        statistics.totalWriteTimeNs.fetch_add(static_cast<std::uint64_t>(writeTime.count()));
    }

    /**
     * @brief Record a dropped record in statistics
     */
    void recordDropped() noexcept {
        statistics.recordsDropped.fetch_add(1);
    }

    /**
     * @brief Record a flush operation in statistics
     */
    void recordFlush() noexcept {
        statistics.flushCount.fetch_add(1);
    }

private:
    std::atomic<core::LogLevel> minLevel;    ///< Minimum log level
    std::atomic<bool>           enabled;     ///< Sink enabled state
    mutable core::LogStatistics statistics;  ///< Performance statistics
};

/**
 * @brief Shared pointer type for sinks
 */
using SinkPtr = std::shared_ptr<Sink>;

/**
 * @brief Weak pointer type for sinks
 */
using WeakSinkPtr = std::weak_ptr<Sink>;

}  // namespace mddlog::sinks