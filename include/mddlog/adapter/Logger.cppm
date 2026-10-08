/**
 * @brief Logger implementation for medical device logging - C++23 Module
 *
 * Adapter-zone module (ADR-001 Decision 6): SimpleLogger stores sinks and a bounded allocating queue,
 * both allocating. The namespace stays mddlog::core for now: only the module name and file
 * location moved (#32), not the class itself.
 */

export module mddlog.adapter.logger;

import std;
import mddlog.core.loglevel;
import mddlog.core.auditring;
import mddlog.core.diagnosticcontext;
import mddlog.adapter.logrecord;
import mddlog.sinks.sink;

export namespace mddlog::core {

/** @brief Centralized diagnostic budgets; saturation always refuses the newest event. */
struct DiagnosticConfig {
    static constexpr std::size_t defaultMessageCapacity = 1024;
    static constexpr std::size_t defaultFlushCapacity   = 8;
    static constexpr std::size_t defaultMaxRecordBytes  = 4096;
    static constexpr std::size_t defaultSinkCapacity    = 16;
    std::size_t                  messageCapacity        = defaultMessageCapacity;
    std::size_t                  flushCapacity          = defaultFlushCapacity;
    std::size_t                  maxRecordBytes         = defaultMaxRecordBytes;
    std::size_t                  sinkCapacity           = defaultSinkCapacity;
};

/** @brief Observable admission and operation outcomes, independent of diagnostic sinks. */
enum class DiagnosticStatus : std::uint8_t { Success, Filtered, Saturated, Oversized, Stopped, Reentrant, Timeout, SinkFailure, InternalFailure, Unsupported };

/** @brief Identity and outcome for a sink participating in one flush. */
struct DiagnosticSinkResult {
    sinks::SinkPtr sink;
    bool           succeeded = false;
};

/** @brief Success means every participating sink returned from flush without throwing. */
struct DiagnosticFlushResult {
    DiagnosticStatus                  status = DiagnosticStatus::Success;
    std::vector<DiagnosticSinkResult> sinks;
};

/** @brief Consistent queue and lifetime counters; failures are never emitted as diagnostics. */
struct DiagnosticHealth {
    std::uint64_t admitted         = 0;
    std::uint64_t processed        = 0;
    std::uint64_t saturated        = 0;
    std::uint64_t oversized        = 0;
    std::uint64_t stopped          = 0;
    std::uint64_t reentrant        = 0;
    std::uint64_t writeFailures    = 0;
    std::uint64_t flushFailures    = 0;
    std::uint64_t flushAdmitted    = 0;
    std::uint64_t flushCompleted   = 0;
    std::uint64_t flushRefused     = 0;
    std::uint64_t timeouts         = 0;
    std::uint64_t shutdownRefused  = 0;
    std::uint64_t internalFailures = 0;
    std::uint64_t sinkRefused      = 0;
    std::size_t   messages         = 0;
    std::size_t   flushes          = 0;
    std::size_t   messageHighWater = 0;
    std::size_t   flushHighWater   = 0;
    bool          closing          = false;
    bool          shutdownComplete = false;
};

/**
 * @brief Simplified logger class with basic threading support
 *
 * This logger provides thread-safe logging capabilities for medical devices
 * using standard C++ threading primitives for maximum compatibility.
 */
class SimpleLogger {
public:
    using SinkPtr       = sinks::SinkPtr;
    using SinkContainer = std::vector<SinkPtr>;

    /**
     * @brief Constructor
     * @param loggerName Logger name/identifier
     * @param enableAsyncLogging Enable asynchronous logging (default: true)
     */
    explicit SimpleLogger(std::string_view loggerName, bool enableAsyncLogging = true) : SimpleLogger(loggerName, DiagnosticConfig{}, enableAsyncLogging) {}

    /** @brief Configure all bounds before starting the worker; zero budgets are invalid. */
    SimpleLogger(std::string_view loggerName, DiagnosticConfig budgets, bool enableAsyncLogging = true)
        : name(loggerName), asyncLogging(enableAsyncLogging), config(budgets) {
        if (config.messageCapacity == 0 || config.flushCapacity == 0 || config.maxRecordBytes == 0 || config.sinkCapacity == 0
            || config.messageCapacity > std::numeric_limits<std::size_t>::max() - config.flushCapacity)
            throw std::invalid_argument("invalid diagnostic budgets");
        commands.resize(config.messageCapacity + config.flushCapacity);
        stopCompletion = std::make_shared<Completion>();
        if (asyncLogging)
            asyncThread = std::jthread([this] {
                asyncLoggerThread();
            });
    }

    /** @brief Drain admitted work and join; host sinks must eventually return. */
    ~SimpleLogger() {
        startShutdown();
        std::unique_lock lock(stopCompletion->mutex);
        stopCompletion->condition.wait(lock, [this] {
            return stopCompletion->done;
        });
        lock.unlock();
        if (asyncThread.joinable())
            asyncThread.join();
        clearSinks();
    }

    // Disable copy and move for thread safety
    SimpleLogger(const SimpleLogger&)            = delete;
    SimpleLogger& operator=(const SimpleLogger&) = delete;
    SimpleLogger(SimpleLogger&&)                 = delete;
    SimpleLogger& operator=(SimpleLogger&&)      = delete;

    /**
     * @brief Add a sink to the logger
     * @param sink Sink to add
     */
    void addSink(SinkPtr sink) {
        (void)tryAddSink(std::move(sink));
    }

    /** @brief Registration has a fixed budget; duplicates are refused. */
    [[nodiscard]] bool tryAddSink(SinkPtr sink) {
        if (!sink)
            return false;
        std::scoped_lock lock(sinksMutex, queueMutex);
        if (counters.closing || sinks.size() >= config.sinkCapacity || std::ranges::find(sinks, sink) != sinks.end()) {
            ++counters.sinkRefused;
            return false;
        }
        sinks.push_back(std::move(sink));
        return true;
    }

    /** @brief Remove from future snapshots; an engaged snapshot retains shared ownership. */
    void removeSink(std::string_view sinkName) {
        auto candidates = sinkSnapshot();
        std::erase_if(candidates, [sinkName](const SinkPtr& sink) {
            return sink->getName() != sinkName;
        });
        SinkContainer retired;
        {
            std::scoped_lock lock(sinksMutex);
            for (auto it = sinks.begin(); it != sinks.end();) {
                if (std::ranges::find(candidates, *it) != candidates.end()) {
                    retired.push_back(std::move(*it));
                    it = sinks.erase(it);
                } else
                    ++it;
            }
        }
    }

    /** @brief Release ownership outside the registry lock, including user destructors. */
    void clearSinks() {
        SinkContainer retired;
        {
            std::scoped_lock lock(sinksMutex);
            retired.swap(sinks);
        }
    }

    /**
     * @brief Log a message with specified level
     * @param level Log level
     * @param message Log message
     * @param category Category/component name
     * @param loc Caller's source location (auto-filled)
     *
     * @c loc must be an explicit parameter here rather than relying on LogRecord's own
     * defaulted @c std::source_location::current(): a default argument is evaluated at the
     * call site of the function that declares it, so without this parameter every record
     * would capture this line inside Logger, not the application's call to log()/info()/etc.
     */
    void
    log(LogLevel level, std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        (void)tryLog(level, message, category, loc);
    }

    /** @brief Return admission explicitly; compatibility methods retain their void signatures. */
    [[nodiscard]] DiagnosticStatus
    tryLog(LogLevel level, std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        return submit(level, {message, category}, [&] {
            return LogRecord(level, message, category, loc);
        });
    }

    /** @brief Emit diagnostic context into structured fields, retaining the existing adapter clock. */
    void log(LogLevel level, const DiagnosticContext& context, std::string_view message, const std::source_location& loc = std::source_location::current()) {
        (void)submit(level, {message, context.component(), context.operationId(), context.correlationId()}, [&] {
            LogRecord record(level, message, context.component(), loc);
            record.operationId   = context.operationId();
            record.correlationId = context.correlationId();
            return record;
        });
    }

    /** @brief Snapshot diagnostic enablement and threshold; a concurrent change is not transactional. */
    [[nodiscard]] bool is(LogLevel level) const noexcept {
        return enabled.load() && level >= minLevel.load();
    }

    /**
     * @brief Log a medical compliance record
     * @param level Log level
     * @param message Log message
     * @param category Category/component name
     * @param userId User identifier
     * @param sessionId Session identifier
     * @param deviceId Device identifier
     * @param loc Caller's source location (auto-filled)
     */
    void logMedical(LogLevel                    level,
                    std::string_view            message,
                    std::string_view            category,
                    std::string_view            userId,
                    std::string_view            sessionId,
                    std::string_view            deviceId,
                    const std::source_location& loc = std::source_location::current()) {
        (void)submit(level, {message, category, userId, sessionId, deviceId}, [&] {
            return LogRecord(level, message, category, userId, sessionId, deviceId, loc);
        });
    }

    /**
     * @brief Bind one exclusively owned audit ring for admission through this logger.
     *
     * The ring must outlive this binding and must not have another producer. Audit calls on
     * this logger are serialized; the consumer may drain the ring independently. Configure
     * AuditSinkAdapter with the same ring and a separate audit sink for hand-off.
     */
    template <std::size_t Capacity>
    void setAuditRing(AuditRing<Capacity>& ring) {
        std::scoped_lock lock(auditMutex);
        auditWriter = [&ring](const AuditInput& input) {
            return ring.tryRecord(input);
        };
    }

    /** @brief Remove the binding before destroying its ring. */
    void clearAuditRing() {
        std::scoped_lock lock(auditMutex);
        auditWriter = {};
    }

    /** @brief Admit a complete audit event to memory, or return an explicit local refusal. */
    [[nodiscard]] AuditWriteResult logAudit(const AuditInput& input) {
        std::scoped_lock lock(auditMutex);
        if (!auditWriter)
            return AuditWriteResult::refused({.reason = AuditRefusalReason::Unconfigured});
        return auditWriter(input);
    }

    // Convenience methods for different log levels
    void trace(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Trace, message, category, loc);
    }

    void debug(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Debug, message, category, loc);
    }

    void info(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Info, message, category, loc);
    }

    void warn(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Warn, message, category, loc);
    }

    void error(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Error, message, category, loc);
    }

    void fatal(std::string_view message, std::string_view category = "default", const std::source_location& loc = std::source_location::current()) {
        log(LogLevel::Fatal, message, category, loc);
    }

    /** @brief Compatibility flush; inspect health or use flushChecked for failures. */
    void flush() {
        (void)flushChecked();
    }

    /** @brief FIFO barrier; waits for earlier admitted messages, then flushes one registry snapshot. */
    [[nodiscard]] DiagnosticFlushResult flushChecked() {
        return flushImpl(std::nullopt);
    }

    /** @brief Bound the asynchronous caller's wait, without cancelling an admitted command. */
    [[nodiscard]] DiagnosticFlushResult flushFor(std::chrono::milliseconds timeout) {
        return flushImpl(timeout);
    }

    /** @brief Close admission and drain once; concurrent callers share the same completion. */
    DiagnosticFlushResult shutdown() {
        if (dispatching) {
            std::scoped_lock lock(queueMutex);
            ++counters.shutdownRefused;
            return {.status = DiagnosticStatus::Reentrant, .sinks = {}};
        }
        startShutdown();
        return awaitCompletion(stopCompletion, std::nullopt);
    }

    /** @brief Async deadline only; destruction still joins and requires returning host sinks. */
    [[nodiscard]] DiagnosticFlushResult shutdownFor(std::chrono::milliseconds timeout) {
        if (dispatching) {
            std::scoped_lock lock(queueMutex);
            ++counters.shutdownRefused;
            return {.status = DiagnosticStatus::Reentrant, .sinks = {}};
        }
        if (!asyncLogging)
            return {.status = DiagnosticStatus::Unsupported, .sinks = {}};
        startShutdown();
        return awaitCompletion(stopCompletion, timeout);
    }

    [[nodiscard]] DiagnosticHealth health() const {
        std::scoped_lock lock(queueMutex);
        return counters;
    }

    /** @brief Lifecycle operations that destroy the logger are forbidden inside host callbacks. */
    [[nodiscard]] static bool inSinkCallback() noexcept {
        return dispatching;
    }

    [[nodiscard]] DiagnosticConfig configuration() const noexcept {
        return config;
    }

    /**
     * @brief Check if logger is enabled
     */
    bool isEnabled() const noexcept {
        return enabled.load();
    }

    /**
     * @brief Enable or disable the logger
     */
    void setEnabled(bool value) noexcept {
        enabled.store(value);
    }

    /**
     * @brief Get minimum log level
     */
    LogLevel getMinLevel() const noexcept {
        return minLevel.load();
    }

    /**
     * @brief Set minimum log level
     */
    void setMinLevel(LogLevel level) noexcept {
        minLevel.store(level);
    }

    /**
     * @brief Get logger name
     */
    std::string_view getName() const noexcept {
        return name;
    }

    /**
     * @brief Check if asynchronous logging is enabled
     */
    bool isAsyncLogging() const noexcept {
        return asyncLogging;
    }

    /**
     * @brief Get number of sinks
     */
    std::size_t getSinkCount() const {
        std::scoped_lock lock(sinksMutex);
        return sinks.size();
    }

private:
    mutable std::mutex                                 auditMutex;
    std::function<AuditWriteResult(const AuditInput&)> auditWriter;
    struct Completion {
        std::mutex              mutex;
        std::condition_variable condition;
        bool                    done = false;
        DiagnosticFlushResult   result;
    };
    struct Command {
        std::optional<LogRecord>    record;
        std::shared_ptr<Completion> completion;
    };
    struct DispatchGuard {
        DispatchGuard() {
            dispatching = true;
        }
        ~DispatchGuard() {
            dispatching = previous;
        }
        DispatchGuard(const DispatchGuard&)            = delete;
        DispatchGuard& operator=(const DispatchGuard&) = delete;
        DispatchGuard(DispatchGuard&&)                 = delete;
        DispatchGuard& operator=(DispatchGuard&&)      = delete;
        bool           previous                        = dispatching;
    };

    template <typename Factory>
    DiagnosticStatus submit(LogLevel level, std::initializer_list<std::string_view> fields, Factory factory) {
        if (!is(level))
            return DiagnosticStatus::Filtered;
        if (dispatching) {
            std::scoped_lock lock(queueMutex);
            ++counters.reentrant;
            return DiagnosticStatus::Reentrant;
        }
        std::unique_lock deliveryLock(deliveryMutex, std::defer_lock);
        if (!asyncLogging)
            deliveryLock.lock();
        // Hold admission while constructing: concurrent producers cannot accumulate allocated
        // records outside the budget. Disabled, closed, oversized and saturated calls allocate none.
        std::unique_lock lock(queueMutex);
        if (counters.closing) {
            ++counters.stopped;
            return DiagnosticStatus::Stopped;
        }
        std::size_t remaining = config.maxRecordBytes;
        for (auto field : fields) {
            if (field.size() > remaining) {
                ++counters.oversized;
                return DiagnosticStatus::Oversized;
            }
            remaining -= field.size();
        }
        if (counters.messages == config.messageCapacity) {
            ++counters.saturated;
            return DiagnosticStatus::Saturated;
        }
        auto record = factory();
        ++counters.admitted;
        ++counters.messages;
        counters.messageHighWater = std::max(counters.messageHighWater, counters.messages);
        if (asyncLogging) {
            push({.record = std::move(record), .completion = {}});
            lock.unlock();
            queueCondition.notify_one();
        } else {
            lock.unlock();
            writeToSinks(record);
            std::scoped_lock healthLock(queueMutex);
            --counters.messages;
            ++counters.processed;
            queueCondition.notify_all();
        }
        return DiagnosticStatus::Success;
    }

    SinkContainer sinkSnapshot() const {
        std::scoped_lock lock(sinksMutex);
        return sinks;
    }

    void writeToSinks(const LogRecord& record) {
        DispatchGuard guard;
        try {
            for (const auto& sink : sinkSnapshot()) {
                if (!sink->shouldLog(record.level) || !sink->isEnabled())
                    continue;
                const auto before = sink->getStatistics().recordsDropped.load();
                bool       threw  = false;
                try {
                    sink->write(record);
                } catch (...) {
                    threw = true;
                    if (sink->getStatistics().recordsDropped.load() == before)
                        sink->recordWriteFailure();
                }
                const auto after = sink->getStatistics().recordsDropped.load();
                if (after > before || threw) {
                    std::scoped_lock lock(queueMutex);
                    counters.writeFailures += after > before ? after - before : 1;
                }
            }
        } catch (...) {
            std::scoped_lock lock(queueMutex);
            ++counters.internalFailures;
        }
    }

    DiagnosticFlushResult flushSinks() {
        DispatchGuard         guard;
        DiagnosticFlushResult result;
        SinkContainer         snapshot;
        try {
            snapshot = sinkSnapshot();
            result.sinks.resize(snapshot.size());
        } catch (...) {
            std::scoped_lock lock(queueMutex);
            ++counters.internalFailures;
            return {.status = DiagnosticStatus::InternalFailure, .sinks = {}};
        }
        std::size_t index = 0;
        for (const auto& sink : snapshot) {
            bool       succeeded = true;
            const auto before    = sink->getStatistics().flushFailures.load();
            try {
                sink->flush();
            } catch (...) {
                result.status = DiagnosticStatus::SinkFailure;
                succeeded     = false;
                if (sink->getStatistics().flushFailures.load() == before)
                    sink->recordFlushFailure();
            }
            const auto after = sink->getStatistics().flushFailures.load();
            if (after > before || !succeeded) {
                succeeded     = false;
                result.status = DiagnosticStatus::SinkFailure;
                std::scoped_lock lock(queueMutex);
                counters.flushFailures += after > before ? after - before : 1;
            }
            result.sinks[index++] = {.sink = sink, .succeeded = succeeded};
        }
        return result;
    }

    void push(Command command) {
        commands[(head + size) % commands.size()] = std::move(command);
        ++size;
    }

    Command pop() {
        auto command   = std::move(commands[head]);
        commands[head] = {};
        head           = (head + 1) % commands.size();
        --size;
        return command;
    }

    static void complete(const std::shared_ptr<Completion>& completion, DiagnosticFlushResult result) {
        {
            std::scoped_lock lock(completion->mutex);
            completion->result = std::move(result);
            completion->done   = true;
        }
        completion->condition.notify_all();
    }

    DiagnosticFlushResult awaitCompletion(const std::shared_ptr<Completion>& completion, std::optional<std::chrono::milliseconds> timeout) {
        std::unique_lock lock(completion->mutex);
        if (timeout) {
            if (!completion->condition.wait_for(lock, *timeout, [&] {
                    return completion->done;
                })) {
                std::scoped_lock healthLock(queueMutex);
                ++counters.timeouts;
                return {.status = DiagnosticStatus::Timeout, .sinks = {}};
            }
        } else
            completion->condition.wait(lock, [&] {
                return completion->done;
            });
        return completion->result;
    }

    DiagnosticFlushResult flushImpl(std::optional<std::chrono::milliseconds> timeout) {
        if (dispatching) {
            std::scoped_lock lock(queueMutex);
            ++counters.flushRefused;
            return {.status = DiagnosticStatus::Reentrant, .sinks = {}};
        }
        if (timeout && !asyncLogging)
            return {.status = DiagnosticStatus::Unsupported, .sinks = {}};
        std::unique_lock deliveryLock(deliveryMutex, std::defer_lock);
        if (!asyncLogging)
            deliveryLock.lock();
        std::shared_ptr<Completion> completion;
        {
            std::scoped_lock lock(queueMutex);
            if (counters.closing) {
                ++counters.flushRefused;
                return {.status = DiagnosticStatus::Stopped, .sinks = {}};
            }
            if (counters.flushes == config.flushCapacity) {
                ++counters.flushRefused;
                return {.status = DiagnosticStatus::Saturated, .sinks = {}};
            }
            if (asyncLogging)
                completion = std::make_shared<Completion>();
            ++counters.flushAdmitted;
            ++counters.flushes;
            counters.flushHighWater = std::max(counters.flushHighWater, counters.flushes);
            if (asyncLogging)
                push({.record = {}, .completion = completion});
        }
        if (asyncLogging) {
            queueCondition.notify_one();
            return awaitCompletion(completion, timeout);
        }
        auto result = flushSinks();
        {
            std::scoped_lock lock(queueMutex);
            --counters.flushes;
            ++counters.flushCompleted;
            queueCondition.notify_all();
        }
        return result;
    }

    void asyncLoggerThread() {
        for (;;) {
            std::unique_lock lock(queueMutex);
            queueCondition.wait(lock, [this] {
                return size != 0 || counters.closing;
            });
            if (size == 0)
                break;
            auto command = pop();
            lock.unlock();
            if (command.record) {
                writeToSinks(*command.record);
                lock.lock();
                --counters.messages;
                ++counters.processed;
            } else {
                auto result = flushSinks();
                lock.lock();
                --counters.flushes;
                ++counters.flushCompleted;
                lock.unlock();
                complete(command.completion, std::move(result));
            }
        }
        finishShutdown();
    }

    void finishShutdown() {
        auto result = flushSinks();
        {
            std::scoped_lock lock(queueMutex);
            counters.shutdownComplete = true;
        }
        complete(stopCompletion, std::move(result));
    }

    void startShutdown() {
        bool first = false;
        {
            std::scoped_lock lock(queueMutex);
            first            = !counters.closing;
            counters.closing = true;
        }
        queueCondition.notify_all();
        if (!asyncLogging && first) {
            std::unique_lock lock(queueMutex);
            queueCondition.wait(lock, [this] {
                return counters.messages == 0 && counters.flushes == 0;
            });
            lock.unlock();
            std::scoped_lock deliveryLock(deliveryMutex);
            finishShutdown();
        }
    }

    std::string           name;
    bool                  asyncLogging;
    DiagnosticConfig      config;
    std::atomic<bool>     enabled{true};
    std::atomic<LogLevel> minLevel{LogLevel::Info};
    mutable std::mutex    sinksMutex;
    SinkContainer         sinks;
    // std::jthread joins only on destruction; shutdown deadlines never detach this object's worker.
    mutable std::mutex              queueMutex;
    std::mutex                      deliveryMutex;
    std::condition_variable         queueCondition;
    DiagnosticHealth                counters;
    std::vector<Command>            commands;
    std::size_t                     head = 0;
    std::size_t                     size = 0;
    std::shared_ptr<Completion>     stopCompletion;
    inline static thread_local bool dispatching = false;
    std::jthread                    asyncThread;
};

}  // namespace mddlog::core
