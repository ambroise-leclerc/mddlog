/** @brief Bounded v1 wire and durable-state encoding for the Linux witness (#115). */
export module mddlog.adapter.witnesscodec;
import std;
import mddlog.core.auditevent;
export import mddlog.adapter.auditanchor;

export namespace mddlog::adapter::witness {
inline constexpr std::size_t   maxBytes        = std::size_t{1024} * 1024;
inline constexpr std::size_t   maxStreams      = 4096;
inline constexpr std::size_t   maxTextBytes    = 1024;
inline constexpr std::uint64_t protocolVersion = 1;
inline constexpr unsigned      byteBits        = 8;
enum class Operation : std::uint8_t { Advance = 1, Retire, Latest, Streams };
enum class Reply : std::uint8_t { Accepted = 1, PositionNotIncreasing, Conflict, Malformed, UnknownStream, Unavailable, Denied, Listing };

/** @brief Explicit little-endian integers and length-prefixed byte strings, bounded before allocation. */
class Encoder {
public:
    void number(std::uint64_t value) {
        if (bytes.size() > maxBytes - sizeof(value))
            throw std::length_error("witness frame limit");
        for (unsigned i = 0; i < sizeof(value); ++i) {
            bytes.push_back(static_cast<std::uint8_t>(value));
            value >>= byteBits;
        }
    }
    void text(std::string_view value) {
        if (value.empty() || value.size() > maxTextBytes || value.contains('\0'))
            throw std::invalid_argument("witness identity");
        number(value.size());
        if (value.size() > maxBytes - bytes.size())
            throw std::length_error("witness frame limit");
        for (const char byte : value)
            bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    void raw(std::span<const std::uint8_t> value) {
        if (value.size() > maxBytes - bytes.size())
            throw std::length_error("witness frame limit");
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void time(core::RawTime value) {
        const bool available = value.availability() == core::TimeAvailability::Available;
        number(available ? 1 : 0);
        number(available ? std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(value.value().time_since_epoch().count())) : 0);
    }
    void anchor(const Anchor& value) {
        number(value.anchorFormat);
        number(value.canonicalVersion);
        text(value.streamId);
        number(value.position);
        raw(value.digest);
        text(value.providerId);
        number(value.counter);
        time(value.acceptedTime);
    }
    void listing(const ProviderListing& value) {
        text(value.providerId);
        number(value.head);
        number(value.entries.size());
        for (const auto& entry : value.entries) {
            const auto* retired = std::get_if<Retirement>(&entry);
            number(retired != nullptr ? 1 : 0);
            anchor(retired != nullptr ? retired->finalAnchor : std::get<Anchor>(entry));
            if (retired != nullptr) {
                number(retired->counter);
                time(retired->retiredTime);
            }
        }
    }
    std::vector<std::uint8_t> bytes;
};
class Decoder {
public:
    explicit Decoder(std::span<const std::uint8_t> input) : bytes(input) {}
    [[nodiscard]] std::uint64_t number() {
        const auto    value = raw(sizeof(std::uint64_t));
        std::uint64_t out   = 0;
        for (unsigned i = 0; i < value.size(); ++i)
            out |= std::uint64_t{value[i]} << (byteBits * i);
        return out;
    }
    [[nodiscard]] Operation operation() {
        const auto value = number();
        if (value < static_cast<std::uint64_t>(Operation::Advance) || value > static_cast<std::uint64_t>(Operation::Streams))
            throw std::invalid_argument("witness operation");
        return static_cast<Operation>(value);
    }
    [[nodiscard]] Reply reply() {
        const auto value = number();
        if (value < static_cast<std::uint64_t>(Reply::Accepted) || value > static_cast<std::uint64_t>(Reply::Listing))
            throw std::invalid_argument("witness reply");
        return static_cast<Reply>(value);
    }
    [[nodiscard]] std::uint16_t version() {
        const auto value = number();
        if (value == 0 || value > std::numeric_limits<std::uint16_t>::max())
            throw std::invalid_argument("witness version");
        return static_cast<std::uint16_t>(value);
    }
    [[nodiscard]] std::span<const std::uint8_t> raw(std::size_t size) {
        if (size > bytes.size() - offset)
            throw std::invalid_argument("witness truncated frame");
        const auto out = bytes.subspan(offset, size);
        offset        += size;
        return out;
    }
    [[nodiscard]] std::string text() {
        const auto size = number();
        if (size == 0 || size > maxTextBytes)
            throw std::invalid_argument("witness text limit");
        const auto  value = raw(static_cast<std::size_t>(size));
        std::string out(value.begin(), value.end());
        if (out.contains('\0'))
            throw std::invalid_argument("witness identity");
        return out;
    }
    [[nodiscard]] core::RawTime time() {
        const auto available = number();
        const auto value     = number();
        if (available > 1 || (available == 0 && value != 0))
            throw std::invalid_argument("witness time");
        return available == 0
                   ? core::RawTime::unavailable()
                   : core::RawTime::available(std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{std::bit_cast<std::int64_t>(value)}});
    }
    [[nodiscard]] Anchor anchor() {
        Anchor out;
        out.anchorFormat     = version();
        out.canonicalVersion = version();
        out.streamId         = text();
        out.position         = number();
        std::ranges::copy(raw(sha256DigestSize), out.digest.begin());
        out.providerId   = text();
        out.counter      = number();
        out.acceptedTime = time();
        return out;
    }
    [[nodiscard]] ProviderListing listing() {
        ProviderListing out{.providerId = text(), .head = number(), .entries = {}};
        const auto      count = number();
        if (count > maxStreams)
            throw std::invalid_argument("witness stream limit");
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto retired = number();
            if (retired > 1)
                throw std::invalid_argument("witness retirement tag");
            auto held = anchor();
            if (retired == 0)
                out.entries.emplace_back(std::move(held));
            else
                out.entries.emplace_back(Retirement{.finalAnchor = std::move(held), .counter = number(), .retiredTime = time()});
        }
        return out;
    }
    void end() const {
        if (offset != bytes.size())
            throw std::invalid_argument("witness trailing bytes");
    }

private:
    std::span<const std::uint8_t> bytes;
    std::size_t                   offset = 0;
};
[[nodiscard]] inline const Anchor& anchorOf(const StreamEntry& entry) {
    if (const auto* retired = std::get_if<Retirement>(&entry))
        return retired->finalAnchor;
    return std::get<Anchor>(entry);
}
[[nodiscard]] inline bool validListing(const ProviderListing& listing, std::string_view identity) {
    if (identity.empty() || identity.size() > maxTextBytes || identity.contains('\0') || listing.providerId != identity || listing.entries.size() > maxStreams)
        return false;
    std::set<std::string>   streams;
    std::set<std::uint64_t> counters;
    std::uint64_t           head = 0;
    for (const auto& entry : listing.entries) {
        const auto& held = anchorOf(entry);
        if (!held.hasMandatoryFields() || !core::AuditEvent::validStreamId(held.streamId) || held.providerId != identity || held.counter > listing.head
            || !streams.insert(held.streamId).second || !counters.insert(held.counter).second)
            return false;
        head = std::max(head, held.counter);
        if (const auto* retired = std::get_if<Retirement>(&entry)) {
            if (retired->counter <= held.counter || retired->counter > listing.head || !counters.insert(retired->counter).second)
                return false;
            head = std::max(head, retired->counter);
        }
    }
    return head == listing.head;
}
}  // namespace mddlog::adapter::witness
