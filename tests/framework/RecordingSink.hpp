/**
 * @brief In-memory test-double sinks used by the SpecLab specs.
 *
 * Include after `import std;` and `import mddlog;`: both RecordingSink and ThrowingSink derive
 * from mddlog::Sink and use mddlog::LogRecord, which must already be visible.
 */
#pragma once

namespace mddlog::spec {

/// Records every accepted LogRecord in memory, with explicit synchronization so the specs can
/// read it from a different thread than the one(s) that wrote to it.
class RecordingSink : public mddlog::Sink {
public:
    explicit RecordingSink(std::string sinkName = "recording", mddlog::LogLevel initialMinLevel = mddlog::LogLevel::TRACE)
        : mddlog::Sink(initialMinLevel), name(std::move(sinkName)) {}

    void write(const mddlog::LogRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex);
        storedRecords.push_back(record);
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(mutex);
        ++numFlushes;
    }

    std::string_view getName() const noexcept override {
        return name;
    }

    std::vector<mddlog::LogRecord> records() const {
        std::lock_guard<std::mutex> lock(mutex);
        return storedRecords;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return storedRecords.size();
    }

    int flushCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return numFlushes;
    }

private:
    std::string                    name;
    mutable std::mutex             mutex;
    std::vector<mddlog::LogRecord> storedRecords;
    int                            numFlushes{0};
};

/// A sink whose write() always throws, for exercising the logger's failure isolation.
class ThrowingSink : public mddlog::Sink {
public:
    explicit ThrowingSink(std::string sinkName = "throwing") : name(std::move(sinkName)) {}

    void write(const mddlog::LogRecord&) override {
        numAttempts.fetch_add(1);
        throw std::runtime_error("ThrowingSink deliberately throws from write()");
    }

    void flush() override {}

    std::string_view getName() const noexcept override {
        return name;
    }

    int attempts() const noexcept {
        return numAttempts.load();
    }

private:
    std::string      name;
    std::atomic<int> numAttempts{0};
};

}  // namespace mddlog::spec
