/** @brief Deterministic overload, failure, callback and lifecycle checks for diagnostic budgets. */
import std;
import mddlog;
import mddlog.log;

#include "../framework/RecordingSink.hpp"

namespace {
using namespace std::chrono_literals;
using mddlog::DiagnosticStatus;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class GateSink : public mddlog::Sink {
public:
    void write(const mddlog::LogRecord&) override {
        if (first) {
            first = false;
            entered.release();
        }
        // A finite host bound also prevents a broken test from hanging destruction indefinitely.
        if (!release.try_acquire_for(5s))
            throw std::runtime_error("gate timeout");
    }
    void             flush() override {}
    std::string_view getName() const noexcept override {
        return "gate";
    }
    bool                        first = true;
    std::binary_semaphore       entered{0};
    std::counting_semaphore<16> release{0};
};

class RejectingSink : public mddlog::Sink {
public:
    void write(const mddlog::LogRecord&) override {
        recordDropped();
    }
    void flush() override {
        recordFlushFailure();
    }
    std::string_view getName() const noexcept override {
        return "rejecting";
    }
};

class FlushThrower : public mddlog::spec::ThrowingSink {
public:
    void flush() override {
        throw std::runtime_error("flush failure");
    }
};

void overload() {
    mddlog::SimpleLogger logger("bounded", {.messageCapacity = 3, .flushCapacity = 2, .maxRecordBytes = 16, .sinkCapacity = 2});
    auto                 gate      = std::make_shared<GateSink>();
    auto                 recording = std::make_shared<mddlog::spec::RecordingSink>();
    logger.addSink(gate);
    logger.addSink(recording);
    require(!logger.tryAddSink(recording), "duplicate registration refused");
    require(!logger.tryAddSink(std::make_shared<RejectingSink>()), "sink capacity enforced");
    require(logger.tryLog(mddlog::LogLevel::Info, "0") == DiagnosticStatus::Success, "first admitted");
    require(gate->entered.try_acquire_for(2s), "worker entered gate");
    require(logger.tryLog(mddlog::LogLevel::Info, "1") == DiagnosticStatus::Success, "second admitted");
    require(logger.tryLog(mddlog::LogLevel::Info, "2") == DiagnosticStatus::Success, "third admitted");
    for (int i = 0; i < 100000; ++i)
        logger.fatal("lost");
    logger.info(std::string(17, 'x'));
    logger.debug("filtered");
    require(logger.flushFor(1ms).status == DiagnosticStatus::Timeout, "first command remains pending");
    require(logger.flushFor(1ms).status == DiagnosticStatus::Timeout, "second command remains pending");
    require(logger.flushFor(1ms).status == DiagnosticStatus::Saturated, "command capacity enforced");
    require(logger.shutdownFor(1ms).status == DiagnosticStatus::Timeout, "reserved stop does not require queue space");
    require(logger.tryLog(mddlog::LogLevel::Info, "late") == DiagnosticStatus::Stopped, "closed admission");
    require(logger.flushChecked().status == DiagnosticStatus::Stopped, "flush after stop refused");
    gate->release.release(3);
    require(logger.shutdown().status == DiagnosticStatus::Success, "drain and final flush succeed");
    const auto records = recording->records();
    require(records.size() == 3 && records[0].message == "0" && records[1].message == "1" && records[2].message == "2", "FIFO without duplicates");
    const auto health = logger.health();
    require(health.admitted == 3 && health.processed == 3 && health.saturated == 100000 && health.oversized == 1 && health.stopped == 1,
            "exact event counters");
    require(health.messageHighWater == 3 && health.flushHighWater == 2 && !health.messages && !health.flushes, "budgets include active work");
    require(health.flushAdmitted == 2 && health.flushCompleted == 2 && health.flushRefused == 2 && health.timeouts == 3 && health.sinkRefused == 2,
            "command counters");
    require(recording->flushCount() == 3, "two FIFO flushes and exactly one stop flush");
}

class ExternalStatisticsThrower : public FlushThrower {
public:
    const mddlog::LogStatistics& getStatistics() const noexcept override {
        return externalStatistics;
    }

private:
    mddlog::LogStatistics externalStatistics;
};

void failures(bool asynchronous) {
    mddlog::SimpleLogger logger("failures", asynchronous);
    auto                 throwing  = std::make_shared<FlushThrower>();
    auto                 rejecting = std::make_shared<RejectingSink>();
    auto                 good      = std::make_shared<mddlog::spec::RecordingSink>();
    logger.addSink(throwing);
    logger.addSink(rejecting);
    logger.addSink(good);
    logger.addSink(std::make_shared<ExternalStatisticsThrower>());
    logger.info("delivered");
    const auto result = logger.flushChecked();
    require(result.status == DiagnosticStatus::SinkFailure && result.sinks.size() == 4, "flush failure visible");
    require(!result.sinks[0].succeeded && !result.sinks[1].succeeded && result.sinks[2].succeeded && !result.sinks[3].succeeded, "per sink outcomes");
    auto health = logger.health();
    require(health.writeFailures == 3 && health.flushFailures == 3 && !health.internalFailures, "separate exception and rejection counters");
    require(throwing->getStatistics().recordsDropped.load() == 1 && throwing->getStatistics().flushFailures.load() == 1, "sink statistics preserved");
    require(good->size() == 1, "healthy sink receives event");
    logger.flush();
    require(logger.health().flushFailures == 6, "void compatibility flush remains observable");
    require(logger.shutdown().status == DiagnosticStatus::SinkFailure, "stop does not imply successful flush");
}

class CallbackSink : public mddlog::Sink {
public:
    CallbackSink(mddlog::SimpleLogger& destination, mddlog::SinkPtr nextSink) : logger(destination), replacement(std::move(nextSink)) {}
    void write(const mddlog::LogRecord&) override {
        admission   = logger.tryLog(mddlog::LogLevel::Error, "recursive");
        flushStatus = logger.flushChecked().status;
        stopStatus  = logger.shutdown().status;
        logger.removeSink(getName());
        logger.addSink(replacement);
    }
    void             flush() override {}
    std::string_view getName() const noexcept override {
        return "callback";
    }
    mddlog::SimpleLogger& logger;
    mddlog::SinkPtr       replacement;
    DiagnosticStatus      admission   = DiagnosticStatus::Success;
    DiagnosticStatus      flushStatus = DiagnosticStatus::Success;
    DiagnosticStatus      stopStatus  = DiagnosticStatus::Success;
};

void callbacks(bool asynchronous) {
    mddlog::SimpleLogger logger("callbacks", asynchronous);
    auto                 good     = std::make_shared<mddlog::spec::RecordingSink>();
    auto                 callback = std::make_shared<CallbackSink>(logger, good);
    logger.addSink(callback);
    logger.info("first");
    require(logger.flushChecked().status == DiagnosticStatus::Success, "callback finishes without locks");
    require(callback->admission == DiagnosticStatus::Reentrant && callback->flushStatus == DiagnosticStatus::Reentrant
                && callback->stopStatus == DiagnosticStatus::Reentrant,
            "recursive operations refused");
    require(logger.getSinkCount() == 1 && logger.health().reentrant == 1 && logger.health().shutdownRefused == 1, "self removal and registration");
    logger.info("second");
    logger.flush();
    require(good->size() == 1, "new sink starts with subsequent snapshot");
    logger.clearSinks();
    require(logger.health().admitted == 2, "health independent of sink ownership");
}

void concurrent() {
    mddlog::SimpleLogger logger("concurrent", {.messageCapacity = 32, .flushCapacity = 4});
    auto                 good = std::make_shared<mddlog::spec::RecordingSink>();
    logger.addSink(good);
    std::barrier              start(7);
    std::latch                firstHalf(4);
    std::vector<std::jthread> threads;
    for (int p = 0; p < 4; ++p)
        threads.emplace_back([&, p] {
            start.arrive_and_wait();
            for (int i = 0; i < 2000; ++i) {
                logger.info(std::format("{}:{}", p, i));
                if (i == 999)
                    firstHalf.count_down();
            }
        });
    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < 50; ++i)
            (void)logger.flushFor(1ms);
    });
    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < 100; ++i) {
            auto temporary = std::make_shared<RejectingSink>();
            logger.addSink(temporary);
            logger.removeSink("rejecting");
        }
    });
    start.arrive_and_wait();
    firstHalf.wait();
    (void)logger.shutdown();
    threads.clear();
    const auto health = logger.health();
    require(health.admitted + health.saturated + health.stopped == 8000, "all concurrent emissions accounted for");
    require(health.admitted == health.processed && good->size() == health.admitted, "all admitted events drained");
    require(health.messages == 0 && health.flushes == 0 && health.shutdownComplete, "stop completed all commands");
    require(health.messageHighWater <= 32 && health.flushHighWater <= 4, "concurrent bounds");
    std::set<std::string> unique;
    for (const auto& record : good->records())
        require(unique.insert(record.message).second, "no duplication");
}

class OperationSink : public mddlog::Sink {
public:
    void write(const mddlog::LogRecord& record) override {
        operations.push_back(record.message);
    }
    void flush() override {
        operations.emplace_back("flush");
    }
    std::string_view getName() const noexcept override {
        return "operations";
    }
    std::vector<std::string> operations;
};

void order() {
    mddlog::SimpleLogger logger("order", {.messageCapacity = 2, .flushCapacity = 1});
    auto                 gate       = std::make_shared<GateSink>();
    auto                 operations = std::make_shared<OperationSink>();
    auto                 removed    = std::make_shared<mddlog::spec::RecordingSink>("removed");
    logger.addSink(gate);
    logger.addSink(operations);
    logger.addSink(removed);
    logger.info("first");
    require(gate->entered.try_acquire_for(2s), "first snapshot engaged");
    logger.removeSink("removed");
    require(logger.flushFor(1ms).status == DiagnosticStatus::Timeout, "barrier admitted while first record blocked");
    logger.info("second");
    gate->release.release(2);
    (void)logger.shutdown();
    require(operations->operations == std::vector<std::string>{"first", "flush", "second", "flush"}, "later records cannot overtake FIFO flush");
    require(removed->size() == 1, "engaged snapshot retains removed sink");
}

class FlushGateSink : public mddlog::Sink {
public:
    void write(const mddlog::LogRecord&) override {
        ++writes;
    }
    void flush() override {
        if (++flushes == 1) {
            entered.release();
            if (!release.try_acquire_for(5s))
                throw std::runtime_error("flush gate timeout");
        }
    }
    std::string_view getName() const noexcept override {
        return "flush-gate";
    }
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    int                   writes  = 0;
    int                   flushes = 0;
};

void activeFlush() {
    mddlog::SimpleLogger logger("active-flush", {.messageCapacity = 1, .flushCapacity = 1});
    auto                 gate = std::make_shared<FlushGateSink>();
    logger.addSink(gate);
    DiagnosticStatus outcome = DiagnosticStatus::SinkFailure;
    std::jthread     flushing([&] {
        outcome = logger.flushChecked().status;
    });
    require(gate->entered.try_acquire_for(2s), "flush engaged");
    require(logger.flushFor(1ms).status == DiagnosticStatus::Saturated, "active command retains its budget");
    logger.info("later");
    require(logger.shutdownFor(1ms).status == DiagnosticStatus::Timeout, "close while flush engaged");
    gate->release.release();
    flushing.join();
    require(logger.shutdown().status == DiagnosticStatus::Success && outcome == DiagnosticStatus::Success, "flush and stop both complete");
    require(gate->writes == 1 && gate->flushes == 2 && logger.health().flushCompleted == 1, "no forgotten command or duplicate final flush");
}

void configuration() {
    bool rejected = false;
    try {
        mddlog::SimpleLogger invalid("invalid", {.messageCapacity = 0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "zero capacity rejected");
    mddlog::SimpleLogger logger("fields", {.maxRecordBytes = 6}, false);
    require(logger.tryLog(mddlog::LogLevel::Info, "123", "456") == DiagnosticStatus::Success, "exact byte boundary admitted");
    require(logger.tryLog(mddlog::LogLevel::Info, "1234", "456") == DiagnosticStatus::Oversized, "one byte beyond boundary refused");
    logger.logMedical(mddlog::LogLevel::Info, "1", "2", "3", "4", "567");
    require(logger.health().oversized == 2, "all medical fields included");
    require(logger.flushFor(1ms).status == DiagnosticStatus::Unsupported && logger.shutdownFor(1ms).status == DiagnosticStatus::Unsupported,
            "no fabricated synchronous deadlines");
    require(logger.health().admitted == 1 && !logger.health().closing, "unsupported deadline does not close admission");
}

class FailingBuffer : public std::streambuf {
public:
    int sync() override {
        return -1;
    }
    std::streamsize xsputn(const char*, std::streamsize) override {
        return 0;
    }
    int_type overflow(int_type) override {
        return traits_type::eof();
    }
};

void console() {
    mddlog::SimpleLogger logger("console-errors", false);
    auto                 sink = std::make_shared<mddlog::ConsoleSink>(false, false);
    logger.addSink(sink);
    FailingBuffer buffer;
    auto*         original           = std::cout.rdbuf(&buffer);
    const auto    originalExceptions = std::cout.exceptions();
    const auto    originalErrorState = std::cerr.rdstate();
    logger.info("fails without an exception mask");
    const auto silent = logger.flushChecked();
    std::cout.clear();
    std::cout.exceptions(std::ios_base::badbit | std::ios_base::failbit);
    logger.info("fails with exceptions enabled");
    const auto throwing = logger.flushChecked();
    std::cout.exceptions(std::ios_base::goodbit);
    std::cout.rdbuf(original);
    std::cout.clear();
    std::cout.exceptions(originalExceptions);
    std::cerr.clear(originalErrorState);
    require(silent.status == DiagnosticStatus::SinkFailure && throwing.status == DiagnosticStatus::SinkFailure,
            "stream errors are visible with either exception mask");
    require(logger.health().writeFailures == 2 && logger.health().flushFailures == 2, "console failures counted independently");
    require(sink->getStatistics().recordsWritten.load() == 0 && sink->getStatistics().recordsDropped.load() == 2
                && sink->getStatistics().flushFailures.load() == 2,
            "console statistics do not invent success");
    require(logger.shutdown().status == DiagnosticStatus::Success, "restored streams permit final flush");
}

void global() {
    mddlog::Log::shutdown();
    require(!mddlog::Log::health(), "health does not initialize console");
    mddlog::Log::initialize("global-budget", {.messageCapacity = 2, .maxRecordBytes = 8}, false);
    auto retained = mddlog::Log::getLogger();
    retained->clearSinks();
    mddlog::Log::info("too long for budget");
    require(mddlog::Log::health()->oversized == 1, "facade configured budget");
    mddlog::Log::shutdown();
    require(retained->tryLog(mddlog::LogLevel::Info, "x") == DiagnosticStatus::Stopped, "retained handle closed");
    require(!mddlog::Log::health(), "facade detached");
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): test runner reports failures through exit status.
int main(int argc, char** argv) {
    try {
        require(argc == 2, "scenario argument required");
        const std::string_view scenario(argv[1]);
        if (scenario == "overload")
            overload();
        else if (scenario == "failures-sync")
            failures(false);
        else if (scenario == "failures-async")
            failures(true);
        else if (scenario == "callbacks-sync")
            callbacks(false);
        else if (scenario == "callbacks-async")
            callbacks(true);
        else if (scenario == "concurrent")
            concurrent();
        else if (scenario == "order")
            order();
        else if (scenario == "active-flush")
            activeFlush();
        else if (scenario == "configuration")
            configuration();
        else if (scenario == "console")
            console();
        else if (scenario == "global")
            global();
        else
            throw std::runtime_error("unknown scenario");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
