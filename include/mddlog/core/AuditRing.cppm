/** @brief Separate bounded SPSC admission lane for runtime audit events. */

export module mddlog.core.auditring;

import std;
export import mddlog.core.auditevent;

export namespace mddlog::core {

/**
 * @brief One producer's audit stream and bounded, refuse-new memory queue.
 *
 * The host supplies a globally distinct identity for every producer instance and boot session.
 * Exactly one producer calls tryRecord(), and one consumer calls drain()/acknowledge(). A new
 * producer instance must use a new identity if its sequence starts again at one.
 */
template <std::size_t Capacity>
class AuditRing {
    static_assert(Capacity > 0 && Capacity <= std::numeric_limits<std::uint64_t>::max() / 2);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    class DrainView {
    public:
        [[nodiscard]] constexpr std::span<const AuditEvent> first() const noexcept {
            return firstSpan;
        }
        [[nodiscard]] constexpr std::span<const AuditEvent> second() const noexcept {
            return secondSpan;
        }
        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return firstSpan.size() + secondSpan.size();
        }
        [[nodiscard]] constexpr bool empty() const noexcept {
            return size() == 0;
        }

    private:
        friend class AuditRing;
        constexpr DrainView(const AuditRing*            source,
                            std::uint64_t               start,
                            std::span<const AuditEvent> firstPart,
                            std::span<const AuditEvent> secondPart) noexcept
            : owner(source), readCursor(start), firstSpan(firstPart), secondSpan(secondPart) {}
        const AuditRing*            owner;
        std::uint64_t               readCursor;
        std::span<const AuditEvent> firstSpan;
        std::span<const AuditEvent> secondSpan;
    };

    explicit constexpr AuditRing(std::string_view identity) noexcept : identityValid(streamId.assignExact(identity) && !identity.empty()) {
        if (identityValid) {
            for (char ch : identity) {
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == ':' || ch == '/'
                      || ch == '-'))
                    identityValid = false;
            }
        }
    }
    AuditRing(const AuditRing&)            = delete;
    AuditRing& operator=(const AuditRing&) = delete;
    AuditRing(AuditRing&&)                 = delete;
    AuditRing& operator=(AuditRing&&)      = delete;

    /** @brief Admit to memory, or refuse without consuming a sequence or overwriting a slot. */
    [[nodiscard]] AuditWriteResult tryRecord(const AuditInput& input) noexcept {
        if (!identityValid)
            return AuditWriteResult::refused({AuditRefusalReason::InvalidStream, AuditField::StreamId});
        if (auto failure = AuditEvent::validate(input, streamId.view()); failure.has_value())
            return AuditWriteResult::refused(*failure);
        if (nextSequence == 0)
            return AuditWriteResult::refused({AuditRefusalReason::SequenceExhausted});

        const std::uint64_t write = writeCursor.load(std::memory_order_relaxed);
        const std::uint64_t read  = readCursor.load(std::memory_order_acquire);
        if (write - read >= Capacity) {
            refusals.fetch_add(1, std::memory_order_relaxed);
            return AuditWriteResult::refused({AuditRefusalReason::RingFull});
        }
        const auto result = slots[static_cast<std::size_t>(write % Capacity)].assign(input, streamId.view(), nextSequence);
        if (!result.wasAdmitted())
            return result;
        ++nextSequence;  // Unsigned wrap to zero permanently marks exhaustion.
        writeCursor.store(write + 1, std::memory_order_release);
        return result;
    }

    [[nodiscard]] DrainView drain() noexcept {
        const std::uint64_t               read      = readCursor.load(std::memory_order_relaxed);
        const std::uint64_t               write     = writeCursor.load(std::memory_order_acquire);
        const auto                        unread    = static_cast<std::size_t>(write - read);
        const auto                        start     = static_cast<std::size_t>(read % Capacity);
        const auto                        firstSize = std::min(unread, Capacity - start);
        const std::span<const AuditEvent> all{slots};
        return {this, read, all.subspan(start, firstSize), all.first(unread - firstSize)};
    }

    /** @brief Release only a prefix of the current view; a positive release invalidates it. */
    [[nodiscard]] bool acknowledge(const DrainView& view, std::size_t count) noexcept {
        const std::uint64_t read = readCursor.load(std::memory_order_relaxed);
        if (view.owner != this || view.readCursor != read || count > view.size())
            return false;
        if (count > 0)
            readCursor.store(read + count, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t refusalCount() const noexcept {
        return refusals.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t admittedCount() const noexcept {
        return writeCursor.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t acknowledgedCount() const noexcept {
        return readCursor.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool hasValidIdentity() const noexcept {
        return identityValid;
    }

private:
    std::array<AuditEvent, Capacity>  slots{};
    InlineString<auditStreamCapacity> streamId;
    bool                              identityValid;
    std::uint64_t                     nextSequence = 1;
    std::atomic<std::uint64_t>        writeCursor{0};
    std::atomic<std::uint64_t>        readCursor{0};
    std::atomic<std::uint64_t>        refusals{0};
};

}  // namespace mddlog::core
