/** @brief Synchronous diagnostic text adapter with independently enabled severity groups. */
export module mddlog.adapter.textlogger;

import std;
import mddlog.core.loglevel;
import mddlog.adapter.sinkregistry;

export namespace mddlog::adapter {

/**
 * @brief Diagnostic-only text delivery through the quiescent callback registry.
 *
 * All groups start disabled. Trace shares Debug's mask and display; Fatal shares Error's.
 * Formatting and delivery allocate and run synchronously in the adapter zone. This is not
 * a governed producer or a transport queue. No audit module or audit admission is reachable.
 */
class TextLogger {
public:
    using Registry = SinkRegistry<void(std::string_view)>;
    using Handle   = Registry::Handle;
    using Callback = Registry::Callback;

    /** @brief Enable one diagnostic group; invalid levels are ignored. */
    void set(core::LogLevel level, bool enabled) noexcept {
        if (const auto index = group(level))
            mask.at(*index).store(enabled);
    }

    /** @brief Query one group; invalid levels are disabled. */
    [[nodiscard]] bool is(core::LogLevel level) const noexcept {
        const auto index = group(level);
        return index && mask.at(*index).load();
    }

    /** @brief Disable every diagnostic group. */
    void disableAll() noexcept {
        for (auto& enabled : mask)
            enabled.store(false);
    }

    /** @brief Register one callback, returning its own quiescent-removal handle. */
    [[nodiscard]] Handle addSink(Callback callback) {
        return registry.add(std::move(callback));
    }

    /** @brief Remove a callback using SinkRegistry's quiescence and self-removal contract. */
    void removeSink(const Handle& handle) {
        registry.remove(handle);
    }

    /**
     * @brief Render a diagnostic line, optionally including the caller's source location.
     * @note The timestamp retains system_clock's fractional precision, as in the existing logger.
     */
    void write(core::LogLevel level, std::string_view text, std::optional<std::source_location> location = std::nullopt) {
        if (!is(level))
            return;
        const auto time   = std::chrono::system_clock::now();
        const auto letter = levelCharacter(level);
        if (location) {
            registry.emit(std::format("[{}] {:%T} | {:16}:{:4} | {}",
                                      letter,
                                      time,
                                      std::filesystem::path(location->file_name()).filename().string(),
                                      location->line(),
                                      text));
        } else {
            registry.emit(std::format("[{}] {:%T} | {}", letter, time, text));
        }
    }

    /** @brief Emit the message and a separate, unprefixed dump under one enablement decision. */
    void writeDump(core::LogLevel level, std::string_view text, std::string_view dump) {
        if (!is(level))
            return;
        registry.emit(std::format("[{}] {:%T} | {}", levelCharacter(level), std::chrono::system_clock::now(), text));
        registry.emit(dump);
    }

private:
    static constexpr std::size_t groupCount = 4;
    static constexpr std::size_t errorGroup = 0;
    static constexpr std::size_t warnGroup  = 1;
    static constexpr std::size_t infoGroup  = 2;
    static constexpr std::size_t debugGroup = 3;

    static constexpr std::optional<std::size_t> group(core::LogLevel level) noexcept {
        switch (level) {
            case core::LogLevel::Error:
            case core::LogLevel::Fatal:
                return errorGroup;
            case core::LogLevel::Warn:
                return warnGroup;
            case core::LogLevel::Info:
                return infoGroup;
            case core::LogLevel::Debug:
            case core::LogLevel::Trace:
                return debugGroup;
        }
        return std::nullopt;
    }

    static constexpr char levelCharacter(core::LogLevel level) noexcept {
        switch (level) {
            case core::LogLevel::Error:
            case core::LogLevel::Fatal:
                return 'E';
            case core::LogLevel::Warn:
                return 'W';
            case core::LogLevel::Info:
                return 'I';
            case core::LogLevel::Debug:
            case core::LogLevel::Trace:
                return 'D';
        }
        return '?';
    }

    std::array<std::atomic<bool>, groupCount> mask{};
    Registry                                  registry;
};

}  // namespace mddlog::adapter
