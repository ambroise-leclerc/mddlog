/** @brief The storage abstraction of ADR-004 Decision 9.2 and an in-memory medium with power-cut injection for tests. Adapter zone only. */

export module mddlog.adapter.auditmedium;

import std;

export namespace mddlog::adapter {

namespace detail {
/** @brief What erased flash reads as. */
inline constexpr std::uint8_t erasedByte = 0xFF;
}  // namespace detail

/** @brief An opaque name for one segment on a medium, valid until the segment is reclaimed. */
using SegmentRef = std::uint64_t;

/** @brief What `open` is asked to create: the segment's identity and its preamble and header frame, already encoded (9.4). */
struct SegmentOpening {
    std::string_view              streamId;
    std::uint32_t                 segmentIndex  = 0;
    std::uint64_t                 firstSequence = 0;
    std::span<const std::uint8_t> bytes;
};

enum class OpenStatus : std::uint8_t { Opened, NoSpace, Failed };
enum class AppendStatus : std::uint8_t { Written, Failed };
/** @brief Unsupported is the answer of a medium that is not eligible for level 3, every time. It is not a failure (9.2). */
enum class SyncAnswer : std::uint8_t { Durable, Failed, Unsupported };

/** @brief Opened means nothing about durability: the preamble and header become durable through the first sync that covers them (9.2). */
struct OpenAnswer {
    OpenStatus status  = OpenStatus::Failed;
    SegmentRef segment = 0;
    /** @brief End offset after the opening bytes. */
    std::uint64_t end = 0;
};

/** @brief Written means nothing about durability: the bytes may still sit in a cache, a buffer or a controller (9.2). */
struct AppendAnswer {
    AppendStatus status = AppendStatus::Failed;
    /** @brief The segment's new end offset. */
    std::uint64_t end = 0;
};

struct SegmentInfo {
    SegmentRef    segment = 0;
    std::uint64_t size    = 0;
};

/**
 * @brief The operations the adapter writes through (9.2). An interface contract only: eligibility for level 3 is a property of an implementation and
 * of its deployment, which this interface cannot establish.
 *
 * `sync` answers Durable only after the medium itself acknowledged persistence of every byte before `offset`, the opening included, and of the segment's
 * existence. A medium that cannot promise that answers Unsupported, never an optimistic Durable. Calls come from the one consumer thread.
 */
class StorageMedium {
public:
    StorageMedium()                                = default;
    StorageMedium(const StorageMedium&)            = delete;
    StorageMedium& operator=(const StorageMedium&) = delete;
    StorageMedium(StorageMedium&&)                 = delete;
    StorageMedium& operator=(StorageMedium&&)      = delete;
    virtual ~StorageMedium()                       = default;

    [[nodiscard]] virtual OpenAnswer   open(const SegmentOpening& opening)                             = 0;
    [[nodiscard]] virtual AppendAnswer append(SegmentRef segment, std::span<const std::uint8_t> bytes) = 0;
    [[nodiscard]] virtual SyncAnswer   sync(SegmentRef segment, std::uint64_t offset)                  = 0;
    /** @brief What the medium holds at the range, possibly fewer bytes than asked; empty when the segment is unavailable. */
    [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> read(SegmentRef segment, std::uint64_t offset, std::uint64_t length) = 0;
    /** @brief Every segment the medium holds, or empty when it cannot say. */
    [[nodiscard]] virtual std::optional<std::vector<SegmentInfo>> segments() = 0;
    /** @brief Remove a whole segment. Only retention calls it (Decision 10). */
    [[nodiscard]] virtual bool reclaim(SegmentRef segment) = 0;
};

/**
 * @brief In-memory medium for tests. It tracks, per segment, the bytes written and the prefix a sync confirmed, so that a power loss can leave exactly
 * what 9.2 allows after the barrier: the confirmed prefix, and any mix of absent, partial, complete or erased bytes beyond it.
 *
 * It lives in process memory, so it is not a medium to rely on. With `eligible` false it answers Unsupported to every sync, like a declared non-eligible
 * backend. Faults are injected by the ordinal of a call, to cut or fail at each write and acknowledgment point.
 */
class InMemoryStorageMedium final : public StorageMedium {
public:
    enum class Operation : std::uint8_t { Open, Append, Sync, Reclaim };

    enum class Effect : std::uint8_t {
        /** @brief The call answers failed and has no effect; the medium stays alive. */
        Fail,
        /** @brief Open only: the call answers noSpace and has no effect. */
        NoSpace,
        /** @brief Power is lost before the call has any effect. */
        CutBefore,
        /** @brief Power is lost partway: `partialBytes` of the call's bytes take effect (open and append: written; sync: persisted). */
        CutPartial,
        /** @brief Power is lost after the call took full effect and before it answered, so the host never learns of it. */
        CutAfter
    };

    /** @brief What the medium keeps of the unconfirmed bytes when power is lost (9.2, condition 3). */
    enum class Unconfirmed : std::uint8_t { Dropped, Kept, Half, Erased };

    struct Fault {
        Operation   operation    = Operation::Append;
        std::size_t ordinal      = 1;  // 1-based, over every call of that operation since construction
        Effect      effect       = Effect::Fail;
        std::size_t partialBytes = 0;
    };

    explicit InMemoryStorageMedium(std::size_t capacity, bool eligibleMedium = true) : segmentCapacity(capacity), eligible(eligibleMedium) {}

    void inject(const Fault& fault) {
        faults.push_back(fault);
    }
    /** @brief Number of calls of `operation` so far, to choose an ordinal. */
    [[nodiscard]] std::size_t calls(Operation operation) const {
        return callCount.at(static_cast<std::size_t>(operation));
    }
    [[nodiscard]] bool poweredOff() const noexcept {
        return dead;
    }

    /**
     * @brief Power returns after a loss, or after an orderly stop: unconfirmed bytes are kept as `policy` says, segments whose creation no sync confirmed
     * are lost under Dropped, every fault is cleared and the medium answers again. The call counts keep running.
     */
    void restart(Unconfirmed policy) {
        std::erase_if(held, [&](const Held& segment) {
            return policy == Unconfirmed::Dropped && !segment.existenceConfirmed;
        });
        for (auto& segment : held) {
            const std::size_t confirmed = std::min<std::size_t>(segment.confirmed, segment.data.size());
            const std::size_t loose     = segment.data.size() - confirmed;
            switch (policy) {
                case Unconfirmed::Dropped:
                    segment.data.resize(confirmed);
                    break;
                case Unconfirmed::Kept:
                    break;
                case Unconfirmed::Half:
                    segment.data.resize(confirmed + (loose / 2));
                    break;
                case Unconfirmed::Erased:
                    std::fill(segment.data.begin() + static_cast<std::ptrdiff_t>(confirmed), segment.data.end(), detail::erasedByte);
                    break;
            }
            segment.confirmed = segment.data.size();
        }
        faults.clear();
        dead = false;
    }

    /** @brief Everything the medium holds for one segment, for a reader that is not the adapter under test. */
    [[nodiscard]] std::vector<std::uint8_t> bytesOf(SegmentRef segment) const {
        const auto* found = find(segment);
        return found != nullptr ? found->data : std::vector<std::uint8_t>{};
    }
    /** @brief The prefix a sync confirmed. */
    [[nodiscard]] std::uint64_t confirmedOf(SegmentRef segment) const noexcept {
        const auto* found = find(segment);
        return found != nullptr ? found->confirmed : 0;
    }
    /** @brief Alter one stored byte, as a medium fault or a deliberate rewrite would. */
    void flipByte(SegmentRef segment, std::size_t offset, std::uint8_t mask = 0x01) {
        if (auto* found = find(segment); found != nullptr && offset < found->data.size())
            found->data[offset] ^= mask;
    }
    void truncate(SegmentRef segment, std::size_t size) {
        if (auto* found = find(segment); found != nullptr && size < found->data.size()) {
            found->data.resize(size);
            found->confirmed = std::min<std::uint64_t>(found->confirmed, size);
        }
    }

    [[nodiscard]] OpenAnswer open(const SegmentOpening& opening) override {
        const auto fault = next(Operation::Open);
        if (dead)
            return {};
        if (fault && fault->effect == Effect::NoSpace)
            return {.status = OpenStatus::NoSpace};
        if (fault && fault->effect == Effect::Fail)
            return {};
        if (fault && fault->effect == Effect::CutBefore) {
            dead = true;
            return {};
        }
        if (held.size() >= segmentCapacity)
            return {.status = OpenStatus::NoSpace};
        Held created;
        created.ref      = ++lastRef;
        std::size_t kept = opening.bytes.size();
        if (fault && fault->effect == Effect::CutPartial)
            kept = std::min(kept, fault->partialBytes);
        created.data.assign(opening.bytes.begin(), opening.bytes.begin() + static_cast<std::ptrdiff_t>(kept));
        held.push_back(std::move(created));
        if (fault) {  // CutPartial or CutAfter: power is lost before the call answers
            dead = true;
            return {};
        }
        return {.status = OpenStatus::Opened, .segment = held.back().ref, .end = held.back().data.size()};
    }

    [[nodiscard]] AppendAnswer append(SegmentRef segment, std::span<const std::uint8_t> bytes) override {
        const auto fault = next(Operation::Append);
        if (dead)
            return {};
        if (fault && fault->effect == Effect::Fail)
            return {};
        if (fault && fault->effect == Effect::CutBefore) {
            dead = true;
            return {};
        }
        auto* found = find(segment);
        if (found == nullptr)
            return {};
        std::size_t kept = bytes.size();
        if (fault && fault->effect == Effect::CutPartial)
            kept = std::min(kept, fault->partialBytes);
        found->data.insert(found->data.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(kept));
        if (fault) {
            dead = true;
            return {};
        }
        return {.status = AppendStatus::Written, .end = found->data.size()};
    }

    [[nodiscard]] SyncAnswer sync(SegmentRef segment, std::uint64_t offset) override {
        const auto fault = next(Operation::Sync);
        if (dead)
            return SyncAnswer::Failed;
        if (!eligible)
            return SyncAnswer::Unsupported;
        if (fault && fault->effect == Effect::Fail)
            return SyncAnswer::Failed;
        if (fault && fault->effect == Effect::CutBefore) {
            dead = true;
            return SyncAnswer::Failed;
        }
        auto* found = find(segment);
        if (found == nullptr)
            return SyncAnswer::Failed;
        std::uint64_t reach = std::min<std::uint64_t>(offset, found->data.size());
        if (fault && fault->effect == Effect::CutPartial)
            reach = std::min<std::uint64_t>(reach, found->confirmed + fault->partialBytes);
        found->confirmed          = std::max(found->confirmed, reach);
        found->existenceConfirmed = found->existenceConfirmed || reach > 0;
        if (fault) {
            dead = true;
            return SyncAnswer::Failed;
        }
        return SyncAnswer::Durable;
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(SegmentRef segment, std::uint64_t offset, std::uint64_t length) override {
        if (dead)
            return std::nullopt;
        const auto* found = find(segment);
        if (found == nullptr)
            return std::nullopt;
        if (offset >= found->data.size())
            return std::vector<std::uint8_t>{};
        const std::uint64_t count = std::min<std::uint64_t>(length, found->data.size() - offset);
        const auto          first = found->data.begin() + static_cast<std::ptrdiff_t>(offset);
        return std::vector<std::uint8_t>(first, first + static_cast<std::ptrdiff_t>(count));
    }

    [[nodiscard]] std::optional<std::vector<SegmentInfo>> segments() override {
        if (dead)
            return std::nullopt;
        std::vector<SegmentInfo> out;
        out.reserve(held.size());
        for (const auto& segment : held)
            out.push_back({.segment = segment.ref, .size = segment.data.size()});
        return out;
    }

    [[nodiscard]] bool reclaim(SegmentRef segment) override {
        const auto fault = next(Operation::Reclaim);
        if (dead)
            return false;
        if (fault && fault->effect == Effect::Fail)
            return false;
        if (fault && fault->effect == Effect::CutBefore) {
            dead = true;
            return false;
        }
        const bool removed = std::erase_if(held,
                                           [&](const Held& item) {
                                               return item.ref == segment;
                                           })
                             != 0;
        if (fault) {  // CutPartial or CutAfter: the segment is gone and power is lost before the call answers
            dead = true;
            return false;
        }
        return removed;
    }

private:
    struct Held {
        SegmentRef                ref = 0;
        std::vector<std::uint8_t> data;
        std::uint64_t             confirmed          = 0;
        bool                      existenceConfirmed = false;
    };

    [[nodiscard]] std::optional<Fault> next(Operation operation) {
        const std::size_t ordinal = ++callCount.at(static_cast<std::size_t>(operation));
        for (const auto& fault : faults) {
            if (fault.operation == operation && fault.ordinal == ordinal)
                return fault;
        }
        return std::nullopt;
    }
    [[nodiscard]] Held* find(SegmentRef segment) noexcept {
        const auto it = std::ranges::find(held, segment, &Held::ref);
        return it != held.end() ? &*it : nullptr;
    }
    [[nodiscard]] const Held* find(SegmentRef segment) const noexcept {
        const auto it = std::ranges::find(held, segment, &Held::ref);
        return it != held.end() ? &*it : nullptr;
    }

    std::size_t                segmentCapacity;
    bool                       eligible;
    bool                       dead    = false;
    SegmentRef                 lastRef = 0;
    std::array<std::size_t, 4> callCount{};
    std::vector<Fault>         faults;
    std::vector<Held>          held;
};

}  // namespace mddlog::adapter
