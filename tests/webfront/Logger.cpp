/** @brief Reference module-consuming TU; the facade header remains independent of modules. */
import std;
import mddlog.core.loglevel;
import mddlog.adapter.textlogger;

#include "tooling/LoggerApi.hpp"

namespace webfront::log {
struct SinkHandle::Registration {
    mddlog::adapter::TextLogger::Handle handle;
};

namespace {
using mddlog::core::LogLevel;

mddlog::adapter::TextLogger& logger() {
    static mddlog::adapter::TextLogger instance;
    return instance;
}

std::optional<LogLevel> diagnosticLevel(LogType level) {
    switch (level) {
        case Error:
            return LogLevel::Error;
        case Warn:
            return LogLevel::Warn;
        case Info:
            return LogLevel::Info;
        case Debug:
            return LogLevel::Debug;
        default:
            return std::nullopt;
    }
}

std::string hexDump(std::span<const std::byte> bytes) {
    constexpr std::size_t  rowWidth       = 16;
    constexpr std::size_t  groupWidth     = 8;
    constexpr unsigned int printableStart = 32;
    constexpr unsigned int printableEnd   = 127;
    std::string            result;
    for (std::size_t address = 0; address < bytes.size(); address += rowWidth) {
        result += std::format("{:08x}", address);
        for (std::size_t index = address; index < address + rowWidth; ++index) {
            if (index % groupWidth == 0)
                result += ' ';
            result += index < bytes.size() ? std::format(" {:02x}", std::to_integer<unsigned int>(bytes[index])) : "   ";
        }
        result += ' ';
        for (std::size_t index = address; index < std::min(address + rowWidth, bytes.size()); ++index) {
            const auto value = std::to_integer<unsigned int>(bytes[index]);
            result          += value >= printableStart && value < printableEnd ? static_cast<char>(value) : '.';
        }
        if (address + rowWidth < bytes.size())
            result += '\n';
    }
    return result;
}
}  // namespace

void set(LogType level, bool enabled) {
    if (level == Disabled)
        logger().disableAll();
    else if (const auto mapped = diagnosticLevel(level))
        logger().set(*mapped, enabled);
}

bool is(LogType level) {
    const auto mapped = diagnosticLevel(level);
    return mapped && logger().is(*mapped);
}

void setLogLevel(LogType level) {
    // WebFront's order is inverse to mddlog's; no enum cast crosses this boundary.
    set(Error, level >= Error);
    set(Warn, level >= Warn);
    set(Info, level >= Info);
    set(Debug, level >= Debug);
}

SinkHandle addSink(std::function<void(std::string_view)> callback) {
    return SinkHandle(std::make_shared<SinkHandle::Registration>(logger().addSink(std::move(callback))));
}

void removeSink(const SinkHandle& handle) {
    if (handle.value)
        logger().removeSink(handle.value->handle);
}

namespace detail {
void write(LogType level, std::string_view text) {
    if (const auto mapped = diagnosticLevel(level))
        logger().write(*mapped, text);
}

void writeDebug(std::string_view text, const std::source_location& location) {
    logger().write(LogLevel::Debug, text, location);
}

void writeHex(std::string_view text, std::span<const std::byte> bytes) {
    logger().writeDump(LogLevel::Info, text, hexDump(bytes));
}
}  // namespace detail
}  // namespace webfront::log
