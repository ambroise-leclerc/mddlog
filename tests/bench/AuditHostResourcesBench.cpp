/** @brief Linux adapter costs: diagnostic context, formatting and synchronous file barriers. */
import std;
import mddlog.adapter.diagnosticbinding;
import mddlog.adapter.textlogger;
import mddlog.adapter.auditstore;
import mddlog.adapter.filestoragemedium;
import mddlog.core.auditring;

namespace {
using Clock = std::chrono::steady_clock;
void emit(std::string_view name, Clock::time_point start, std::uint64_t units) {
    std::cout << name << ',' << std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count() << ',' << units << '\n';
}
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("mddlog-host-budget-" + std::to_string(Clock::now().time_since_epoch().count()));
    Directory() {
        std::filesystem::create_directory(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    }
    Directory(const Directory&)            = delete;
    Directory& operator=(const Directory&) = delete;
    Directory(Directory&&)                 = delete;
    Directory& operator=(Directory&&)      = delete;
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
}  // namespace
int main() try {
    using namespace mddlog::adapter;
    constexpr std::size_t iterations   = 100000;
    const auto            contextStart = Clock::now();
    std::uint64_t         contextBytes = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto correlation = std::to_string(i);
        const auto context     = mddlog::core::DiagnosticContext::create({.component = "pump", .operationId = "prime", .correlationId = correlation});
        if (!context)
            return 2;
        contextBytes += context->component().size();
    }
    emit("context", contextStart, contextBytes);
    const auto        context = mddlog::core::DiagnosticContext::create({.component = "pump", .operationId = "prime", .correlationId = "request/1"});
    TextLogger        text;
    std::uint64_t     emittedBytes = 0;
    const auto        callback     = text.addSink([&](std::string_view line) {
        emittedBytes += line.size();
    });
    DiagnosticBinding logger(text, *context);
    text.set(mddlog::core::LogLevel::Info, true);
    const auto formatStart = Clock::now();
    for (std::size_t i = 0; i < 10000; ++i)
        logger.info("bounded diagnostic message");
    emit("format_context", formatStart, emittedBytes);
    text.removeSink(callback);
    std::cout << "audit_event_bytes,0," << sizeof(mddlog::core::AuditEvent) << '\n';
    std::cout << "audit_ring_8_bytes,0," << sizeof(mddlog::core::AuditRing<8>) << '\n';
    std::cout << "diagnostic_context_bytes,0," << sizeof(mddlog::core::DiagnosticContext) << '\n';
    Directory  directory;
    const auto startup = Clock::now();
    auto       medium  = FileStorageMedium::create(
        {.directory = directory.path, .maxSegments = 32, .maxSegmentBytes = 32768, .maxReadBytes = 4096, .durability = FileDurability::QualifiedFsync});
    if (!medium)
        return 3;
    StorageConfig config;
    config.segmentSize              = 32768;
    config.segmentCount             = 32;
    config.maxProducerStreams       = 1;
    config.sync.recordBound         = 128;
    config.resources.readChunkBytes = 4096;
    auto made                       = PersistingAuditSink::create(**medium, config);
    if (!made)
        return 4;
    emit("file_startup", startup, 1);
    const auto append = Clock::now();
    for (std::size_t i = 1; i <= 4096; ++i) {
        mddlog::core::AuditEvent event;
        if (!event.assign({.action = "file.measure", .target = "fixture"}, "file/producer", i).wasAdmitted() || !(*made)->accept(event))
            return 5;
    }
    emit("file_append_sync", append, 4096);
    const auto close = Clock::now();
    (*made)->close();
    emit("file_close", close, 1);
    if ((*made)->durablePosition("file/producer") != 4096)
        return 6;
    std::size_t descriptors = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
        ++descriptors;
    std::cout << "open_descriptors,0," << descriptors << '\n';
    return 0;
} catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 7;
}
