/** @brief Isolated Linux worker for observed process-stop and real filesystem-failure campaigns. */
#include <cerrno>
#include <csignal>
#include <cstdio>

#include <sys/stat.h>
#include <unistd.h>

import std;
import mddlog.adapter.filestoragemedium;

namespace {
using namespace mddlog::adapter;

constexpr std::string_view baseline = "confirmed-prefix:114:v1";
constexpr std::string_view initial  = "target-prefix:114:v1";
constexpr std::string_view pending  = "pending-bytes:114:v1";

[[nodiscard]] std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

void checkpoint(std::string_view wanted, std::string_view point) {
    if (wanted != point)
        return;
    std::println(R"({{"event":"checkpoint","point":"{}"}})", point);
    (void)std::fflush(stdout);
    (void)::raise(SIGSTOP);
}

struct CampaignCalls final : FileStorageCalls {
    CampaignCalls(std::string_view action, std::string_view point) : operation(action), stopAt(point) {}

    std::ptrdiff_t writeMetadata(int descriptor, std::span<const char> data) override {
        const auto count = FileStorageCalls::writeMetadata(descriptor, stopAt == "metadata.partial" ? data.first(std::min(data.size(), std::size_t{7})) : data);
        if (count > 0)
            checkpoint(stopAt, stopAt == "metadata.partial" ? "metadata.partial" : "metadata.written");
        return count;
    }
    int syncMetadata(int descriptor) override {
        struct stat metadata{};
        if (::fstat(descriptor, &metadata) != 0)
            return -1;
        const bool directory = S_ISDIR(metadata.st_mode);
        checkpoint(stopAt, directory ? "metadata.dir.before" : "metadata.file.before");
        const int result = FileStorageCalls::syncMetadata(descriptor);
        if (result == 0)
            checkpoint(stopAt, directory ? "metadata.dir.after" : "metadata.file.after");
        return result;
    }
    int replaceMetadata(int directory, const char* temporary, const char* destination) override {
        checkpoint(stopAt, "metadata.rename.before");
        const int result = FileStorageCalls::replaceMetadata(directory, temporary, destination);
        if (result == 0)
            checkpoint(stopAt, "metadata.rename.after");
        return result;
    }
    std::ptrdiff_t write(int descriptor, std::span<const std::uint8_t> data) override {
        const std::string_view prefix  = operation == "open" ? "segment" : "append";
        const auto             partial = std::format("{}.partial", prefix);
        const auto             count   = FileStorageCalls::write(descriptor, stopAt == partial ? data.first(std::max(std::size_t{1}, data.size() / 2)) : data);
        if (count > 0)
            checkpoint(stopAt, stopAt == partial ? partial : std::format("{}.written", prefix));
        return count;
    }
    std::ptrdiff_t read(int descriptor, std::span<std::uint8_t> data, std::uint64_t offset) override {
        const auto count = FileStorageCalls::read(descriptor, stopAt == "read.partial" ? data.first(std::max(std::size_t{1}, data.size() / 2)) : data, offset);
        if (count > 0)
            checkpoint(stopAt, "read.partial");
        return count;
    }
    int sync(int descriptor) override {
        struct stat metadata{};
        if (::fstat(descriptor, &metadata) != 0)
            return -1;
        const bool       directory = S_ISDIR(metadata.st_mode);
        std::string_view before    = directory ? "sync.dir.before" : "sync.file.before";
        std::string_view after     = directory ? "sync.dir.after" : "sync.file.after";
        if (operation == "reclaim") {
            before = "reclaim.unlinked";
            after  = "reclaim.dir.after";
        }
        checkpoint(stopAt, before);
        const int result = FileStorageCalls::sync(descriptor);
        if (result == 0)
            checkpoint(stopAt, after);
        return result;
    }
    std::string_view operation;
    std::string_view stopAt;
};

void outcome(FileStorageMedium& medium, bool success, SegmentRef ref = 0, bool durable = false, bool noSpace = false) {
    const auto error = medium.lastError().value_or(FileStorageError{});
    std::println(R"({{"event":"operation","ok":{},"ref":{},"durable":{},"no_space":{},"errno":{},"issue":{},"stopped":{}}})",
                 success,
                 ref,
                 durable,
                 noSpace,
                 error.nativeError,
                 std::to_underlying(error.issue),
                 medium.mutationsStopped());
}

[[nodiscard]] int resources(const FileStorageConfig& config) {
    const auto descriptors = [] {
        return std::ranges::distance(std::filesystem::directory_iterator("/proc/self/fd"));
    };
    const auto before   = descriptors();
    SegmentRef previous = 2;
    for (int repetition = 0; repetition < 64; ++repetition) {
        {
            auto writer = FileStorageMedium::create(config);
            if (!writer)
                return 1;
            const auto opened = (*writer)->open({.bytes = bytes(pending)});
            if (opened.status != OpenStatus::Opened || opened.segment <= previous || (*writer)->sync(opened.segment, opened.end) != SyncAnswer::Durable
                || !(*writer)->reclaim(opened.segment))
                return 1;
            previous = opened.segment;
        }
        auto limited        = config;
        limited.maxSegments = 1;
        const auto rejected = FileStorageMedium::create(limited);
        if (rejected || rejected.error().issue != FileStorageIssue::Inventory || descriptors() != before)
            return 1;
    }
    std::println(R"({{"event":"resources","iterations":64,"before":{},"after":{},"last_ref":{}}})", before, descriptors(), previous);
    return 0;
}

[[nodiscard]] int run(std::filesystem::path directory, std::string_view operation, std::string_view point) {
    CampaignCalls     calls(operation, point);
    FileStorageConfig config{.directory       = std::move(directory),
                             .maxSegments     = 8,
                             .maxSegmentBytes = 131072,
                             .maxReadBytes    = 131072,
                             .durability      = FileDurability::QualifiedFsync};
    if (operation == "inspect" || operation == "read")
        config.access = FileAccess::ReadOnly;
    if (operation == "resources")
        return resources(config);
    auto created = FileStorageMedium::create(config, &calls);
    if (!created) {
        std::println(R"({{"event":"startup-failed","errno":{},"issue":{},"inventory":{}}})",
                     created.error().nativeError,
                     std::to_underlying(created.error().issue),
                     created.error().issue == FileStorageIssue::Inventory);
        return 0;
    }
    auto& medium = **created;
    if (operation == "seed" || operation == "seed-big") {
        for (auto text : {baseline, initial}) {
            const std::vector<std::uint8_t> large(4096, text == baseline ? 'B' : 'T');
            const auto                      opened = medium.open({.bytes = operation == "seed-big" ? std::span<const std::uint8_t>(large) : bytes(text)});
            if (opened.status != OpenStatus::Opened || medium.sync(opened.segment, opened.end) != SyncAnswer::Durable)
                return 1;
        }
        outcome(medium, true, 2, true);
    } else if (operation == "inspect") {
        const auto listing = medium.segments();
        if (!listing)
            return 1;
        for (const auto& segment : *listing) {
            const auto read = medium.read(segment.segment, 0, config.maxReadBytes);
            if (!read)
                return 1;
            std::string hex;
            for (const auto byte : *read)
                hex += std::format("{:02x}", byte);
            std::println(R"({{"event":"segment","ref":{},"hex":"{}"}})", segment.segment, hex);
        }
        std::println(R"({{"event":"inventory","count":{}}})", listing->size());
    } else if (operation == "open" || operation == "open-big") {
        checkpoint(point, "open.before");
        const std::vector<std::uint8_t> large(131072, 'N');
        const auto                      opened = medium.open({.bytes = operation == "open-big" ? std::span<const std::uint8_t>(large) : bytes(pending)});
        outcome(medium, opened.status == OpenStatus::Opened, opened.segment, false, opened.status == OpenStatus::NoSpace);
        checkpoint(point, "open.acknowledged");
    } else if (operation == "append" || operation == "sync" || operation == "append-big") {
        checkpoint(point, "append.before");
        const std::vector<std::uint8_t> large(65536, 'A');
        const auto                      appended = medium.append(2, operation == "append-big" ? std::span<const std::uint8_t>(large) : bytes(pending));
        const bool                      written  = appended.status == AppendStatus::Written;
        const bool                      durable  = operation == "sync" && written && medium.sync(2, appended.end) == SyncAnswer::Durable;
        outcome(medium, operation == "sync" ? durable : written, 2, durable);
        checkpoint(point, operation == "sync" ? "sync.acknowledged" : "append.acknowledged");
    } else if (operation == "reclaim") {
        checkpoint(point, "reclaim.before");
        outcome(medium, medium.reclaim(2), 2);
        checkpoint(point, "reclaim.acknowledged");
    } else if (operation == "read") {
        outcome(medium, medium.read(2, 0, config.maxReadBytes).has_value(), 2);
    } else {
        return 2;
    }
    return 0;
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): main reports all failures as a nonzero campaign result.
int main(int argc, char** argv) {
    try {
        const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
        if (arguments.size() < 3 || arguments.size() > 4)
            return 2;
        return run(arguments[1], arguments[2], arguments.size() == 4 ? arguments[3] : "");
    } catch (const std::exception& error) {
        std::println(stderr, "campaign worker: {}", error.what());
        return 1;
    }
}
