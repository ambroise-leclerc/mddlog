/** @brief Isolated allocation failure after a durable trim preserves retry evidence. */
import std;
import mddlog.adapter.auditstore;

namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): isolated replacement-allocation fixture, never linked into library/specs.
std::atomic<bool> rejectNextAllocation{false};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): proves that the deliberately armed allocation failure was reached.
std::atomic<unsigned> rejectedAllocations{0};
}  // namespace

// NOLINTBEGIN(cppcoreguidelines-no-malloc,hicpp-no-malloc,cppcoreguidelines-owning-memory): replaceable allocation functions need an independent heap source.
void* operator new(std::size_t size) {
    if (rejectNextAllocation.exchange(false)) {
        rejectedAllocations.fetch_add(1);
        throw std::bad_alloc{};
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr)
        return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}
// NOLINTEND(cppcoreguidelines-no-malloc,hicpp-no-malloc,cppcoreguidelines-owning-memory)

namespace {
using namespace mddlog::adapter;
class TrimBarrierMedium final : public StorageMedium {
public:
    InMemoryStorageMedium inner{32};
    bool                  arm = false;
    OpenAnswer            open(const SegmentOpening& opening) override {
        return inner.open(opening);
    }
    AppendAnswer append(SegmentRef segment, std::span<const std::uint8_t> bytes) override {
        return inner.append(segment, bytes);
    }
    SyncAnswer sync(SegmentRef segment, std::uint64_t offset) override {
        const auto answer = inner.sync(segment, offset);
        if (arm && answer == SyncAnswer::Durable) {
            const auto bytes  = inner.bytesOf(segment);
            const auto frames = scanSegment(bytes);
            if (!frames.records.empty()) {
                const auto decoded = decodeCanonical(frames.records.back().canonical);
                if (decoded.status == CanonicalReadStatus::Ok && decoded.record.action == "mddlog.stream.trim") {
                    arm = false;
                    rejectNextAllocation.store(true);
                }
            }
        }
        return answer;
    }
    std::optional<std::vector<std::uint8_t>> read(SegmentRef segment, std::uint64_t offset, std::uint64_t length) override {
        return inner.read(segment, offset, length);
    }
    std::optional<std::vector<SegmentInfo>> segments() override {
        return inner.segments();
    }
    bool reclaim(SegmentRef segment) override {
        return inner.reclaim(segment);
    }
};
std::size_t trimCount(StorageMedium& medium) {
    const auto log = LogAnalysis::read(medium);
    if (log.resourceIssue() != AuditResourceIssue::None)
        throw std::runtime_error("fixture analysis incomplete");
    std::size_t count = 0;
    for (const auto& ledger : log.ledgers())
        count += ledger.trims.size();
    return count;
}
}  // namespace
int main() try {
    TrimBarrierMedium      medium;
    InMemoryAnchorProvider provider{"allocation/witness"};
    StorageConfig          config;
    config.segmentSize        = 2048;
    config.segmentCount       = 32;
    config.maxProducerStreams = 1;
    config.provider           = &provider;
    config.ledger             = LedgerConfig{.streamId = "ledger/allocation/confirmed-trim"};
    auto made                 = PersistingAuditSink::create(medium, config);
    if (!made)
        return 2;
    constexpr std::string_view stream = "producer/allocation/confirmed-trim";
    const std::string          detail(200, 'x');
    for (std::uint64_t sequence = 1; sequence <= 40; ++sequence) {
        mddlog::core::AuditEvent event;
        if (!event.assign({.action = "allocation.test", .target = "fixture", .detail = detail}, stream, sequence).wasAdmitted() || !(*made)->accept(event))
            return 3;
    }
    if (!(*made)->closeStream(stream))
        return 4;
    medium.arm             = true;
    const auto interrupted = (*made)->removeStream(stream);
    if (rejectedAllocations.load() != 1 || interrupted.outcome != RetentionOutcome::ResourceLimit || interrupted.trimmedThrough == 0
        || interrupted.segmentsReclaimed != 0 || trimCount(medium) != 1)
        return 5;
    const auto retried = (*made)->removeStream(stream);
    if (retried.outcome != RetentionOutcome::Removed || retried.trimmedThrough != interrupted.trimmedThrough || retried.segmentsReclaimed == 0
        || trimCount(medium) != 1)
        return 6;
    (*made)->close();
    return 0;
} catch (const std::exception& failure) {
    rejectNextAllocation.store(false);
    std::cerr << failure.what() << '\n';
    return 7;
}
