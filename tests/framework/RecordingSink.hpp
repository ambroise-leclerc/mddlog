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
    explicit RecordingSink(std::string name = "recording", mddlog::LogLevel minLevel = mddlog::LogLevel::TRACE)
        : mddlog::Sink(minLevel), name_(std::move(name)) {}

    void write(const mddlog::LogRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(record);
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++flushCount_;
    }

    std::string_view getName() const noexcept override {
        return name_;
    }

    std::vector<mddlog::LogRecord> records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

    int flushCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return flushCount_;
    }

private:
    std::string                    name_;
    mutable std::mutex             mutex_;
    std::vector<mddlog::LogRecord> records_;
    int                            flushCount_{0};
};

/// A sink whose write() always throws, for exercising the logger's failure isolation.
class ThrowingSink : public mddlog::Sink {
public:
    explicit ThrowingSink(std::string name = "throwing") : name_(std::move(name)) {}

    void write(const mddlog::LogRecord&) override {
        attempts_.fetch_add(1);
        throw std::runtime_error("ThrowingSink deliberately throws from write()");
    }

    void flush() override {}

    std::string_view getName() const noexcept override {
        return name_;
    }

    int attempts() const noexcept {
        return attempts_.load();
    }

private:
    std::string      name_;
    std::atomic<int> attempts_{0};
};

}  // namespace mddlog::spec
