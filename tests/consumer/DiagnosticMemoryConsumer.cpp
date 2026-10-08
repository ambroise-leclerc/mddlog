/** @brief Allocation probe with a stopped drain: rejected diagnostic calls retain no new heap storage. */
import std;
import mddlog;

namespace {
std::atomic<bool>        failNextAllocation{false};
std::atomic<bool>        measuring{false};
std::atomic<std::size_t> allocationCount{0};
std::atomic<std::size_t> allocationBytes{0};
}  // namespace

// Probe ordinary C++ allocations used by strings, vectors and shared completions in this adapter.
void* operator new(std::size_t bytes) {
    if (failNextAllocation.exchange(false))
        throw std::bad_alloc();
    auto* memory = std::malloc(bytes ? bytes : 1);
    if (!memory)
        throw std::bad_alloc();
    if (measuring.load()) {
        allocationCount.fetch_add(1);
        allocationBytes.fetch_add(bytes);
    }
    return memory;
}
void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
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

namespace {
using namespace std::chrono_literals;
class BoundedHostSink : public mddlog::Sink {
public:
    void write(const mddlog::LogRecord&) override {
        entered.release();
        if (!release.try_acquire_for(5s))
            throw std::runtime_error("host deadline exceeded");
    }
    void             flush() override {}
    std::string_view getName() const noexcept override {
        return "host";
    }
    std::binary_semaphore      entered{0};
    std::counting_semaphore<2> release{0};
};
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): allocation probe executable; failures return nonzero.
int main() {
    mddlog::SimpleLogger logger("memory", {.messageCapacity = 2, .flushCapacity = 1, .maxRecordBytes = 64, .sinkCapacity = 1});
    auto                 host = std::make_shared<BoundedHostSink>();
    logger.addSink(host);
    const std::string payload(50, 'x');
    logger.info(payload);
    if (!host->entered.try_acquire_for(2s))
        return 1;
    logger.info(payload);
    if (logger.flushFor(1ms).status != mddlog::DiagnosticStatus::Timeout)
        return 1;
    const auto start = std::chrono::steady_clock::now();
    measuring.store(true);
    constexpr int attempts = 1000000;
    for (int i = 0; i < attempts; ++i) {
        logger.fatal(payload);
        logger.debug(payload);
        (void)logger.flushFor(0ms);
    }
    measuring.store(false);
    const auto elapsed    = std::chrono::steady_clock::now() - start;
    const auto atCapacity = logger.health();
    host->release.release(2);
    (void)logger.shutdown();
    std::cout << "attempts=" << attempts << " allocations=" << allocationCount.load() << " allocated_bytes=" << allocationBytes.load()
              << " elapsed_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() << " messages=" << atCapacity.messages
              << " flushes=" << atCapacity.flushes << '\n';
    const bool bounded = allocationCount.load() == 0 && allocationBytes.load() == 0 && atCapacity.saturated == attempts && atCapacity.flushRefused == attempts
                         && atCapacity.messages == 2 && atCapacity.flushes == 1 && logger.health().processed == 2;
    mddlog::SimpleLogger synchronous("allocation-failure", false);
    synchronous.addSink(host);
    failNextAllocation.store(true);
    const auto failure      = synchronous.flushChecked();
    const bool flushFailure = failure.status == mddlog::DiagnosticStatus::InternalFailure && failure.sinks.empty()
                              && synchronous.health().internalFailures == 1;
    failNextAllocation.store(true);
    const auto admissionFailure   = synchronous.tryLog(mddlog::LogLevel::Info, payload);
    const bool admissionUnchanged = admissionFailure == mddlog::DiagnosticStatus::InternalFailure && synchronous.health().internalFailures == 2
                                    && synchronous.health().admitted == 0 && synchronous.health().messages == 0;
    mddlog::SimpleLogger asynchronous("command-allocation");
    failNextAllocation.store(true);
    const auto commandFailure   = asynchronous.flushChecked();
    const bool commandUnchanged = commandFailure.status == mddlog::DiagnosticStatus::InternalFailure && asynchronous.health().internalFailures == 1
                                  && asynchronous.health().flushAdmitted == 0 && asynchronous.health().flushes == 0 && asynchronous.health().flushRefused == 1;
    (void)asynchronous.shutdown();
    // Failure during the final flush is also explicit and cannot strand shutdown.
    failNextAllocation.store(true);
    const auto stopFailure     = synchronous.shutdown();
    const bool shutdownFailure = stopFailure.status == mddlog::DiagnosticStatus::InternalFailure && synchronous.health().shutdownComplete;
    return bounded && flushFailure && admissionUnchanged && commandUnchanged && shutdownFailure ? 0 : 1;
}
