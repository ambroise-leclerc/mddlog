/** @brief Facade declarations shared by the ordinary header and the module-consuming TU.
 * Include after the standard headers in Logger.hpp, or after import std.
 */
#pragma once

namespace webfront::log {

using LogType                     = const std::uint8_t;
inline constexpr LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
inline const auto        clogSink = [](std::string_view text) {
    std::clog << text << '\n';
};

/** @brief Opaque callback registration; contains no module types in the header. */
class SinkHandle {
public:
    SinkHandle() = default;

private:
    struct Registration;
    explicit SinkHandle(std::shared_ptr<Registration> registration) : value(std::move(registration)) {}
    std::shared_ptr<Registration> value;
    friend SinkHandle             addSink(std::function<void(std::string_view)> callback);
    friend void                   removeSink(const SinkHandle& handle);
};

void               set(LogType level, bool enabled);
[[nodiscard]] bool is(LogType level);
/**
 * @brief Update the four independent severity groups in sequence.
 * @note Concurrent is()/write calls may observe a partially applied level change;
 *       there is no atomic update across groups.
 */
void                     setLogLevel(LogType level);
[[nodiscard]] SinkHandle addSink(std::function<void(std::string_view)> callback);
void                     removeSink(const SinkHandle& handle);

namespace detail {
void write(LogType level, std::string_view text);
void writeDebug(std::string_view text, const std::source_location& location);
void writeHex(std::string_view text, std::span<const std::byte> bytes);
}  // namespace detail

template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
struct debug {  // NOLINT(readability-identifier-naming): preserve WebFront's CTAD call-site API.
    debug(std::string_view fmt, Ts&&... args, const std::source_location& location = std::source_location::current()) {
        if (is(Debug))
            detail::writeDebug(std::vformat(fmt, std::make_format_args(args...)), location);
    }
};
template <typename... Ts>
debug(std::string_view, Ts&&...) -> debug<Ts...>;

template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
void info(std::string_view fmt, Ts&&... args) {
    if (is(Info))
        detail::write(Info, std::vformat(fmt, std::make_format_args(args...)));
}
template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
void warn(std::string_view fmt, Ts&&... args) {
    if (is(Warn))
        detail::write(Warn, std::vformat(fmt, std::make_format_args(args...)));
}
template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
void error(std::string_view fmt, Ts&&... args) {
    if (is(Error))
        detail::write(Error, std::vformat(fmt, std::make_format_args(args...)));
}

/** @brief Preserve the second hex-dump write for contiguous numeric or byte buffers. */
template <std::ranges::contiguous_range Container>
    requires std::ranges::sized_range<Container>
             && (std::is_arithmetic_v<std::ranges::range_value_t<Container>> || std::same_as<std::ranges::range_value_t<Container>, std::byte>)
void infoHex(std::string_view text, const Container& container) {
    if (is(Info))
        detail::writeHex(text, std::as_bytes(std::span(std::ranges::data(container), std::ranges::size(container))));
}

template <typename... Callbacks>
    requires(sizeof...(Callbacks) > 0) && (std::constructible_from<std::function<void(std::string_view)>, Callbacks> && ...)
auto addSinks(Callbacks&&... callbacks) {
    if constexpr (sizeof...(Callbacks) == 1)
        return (addSink(std::forward<Callbacks>(callbacks)), ...);
    else
        return std::array<SinkHandle, sizeof...(Callbacks)>{addSink(std::forward<Callbacks>(callbacks))...};
}

template <typename... Handles>
    requires(std::same_as<std::remove_cvref_t<Handles>, SinkHandle> && ...)
void removeSinks(Handles&&... handles) {
    (removeSink(handles), ...);
}

}  // namespace webfront::log
