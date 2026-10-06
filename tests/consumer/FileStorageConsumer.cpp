/** @brief Direct-import consumer of the optional file backend, in source and installed packages. */
#include <cstdlib>

import std;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.fileretainedposition;
import mddlog.adapter.fileanchorauthority;
import mddlog.adapter.unixanchorprovider;

int main() {
    using namespace mddlog::adapter;
    std::string pattern = (std::filesystem::temp_directory_path() / "mddlog-consumer-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr)
        return 1;
    const std::filesystem::path       path(pattern);
    int                               result = 0;
    FileStorageConfig                 config{.directory = path, .maxSegments = 2, .maxSegmentBytes = 128, .maxReadBytes = 128};
    const std::array<std::uint8_t, 3> bytes{1, 2, 3};
    SegmentRef                        ref = 0;
    {
        auto made = FileStorageMedium::create(config);
        if (!made)
            result = 2;
        else {
            const auto opened = (*made)->open({.bytes = bytes});
            ref               = opened.segment;
            if (opened.status != OpenStatus::Opened || (*made)->sync(ref, opened.end) != SyncAnswer::Unsupported)
                result = 3;
        }
    }
    if (result == 0) {
        config.access = FileAccess::ReadOnly;
        auto reader   = FileStorageMedium::create(config);
        if (!reader || (*reader)->read(ref, 0, 3) != std::optional(std::vector<std::uint8_t>(bytes.begin(), bytes.end())))
            result = 4;
    }
    if (result == 0) {
        const auto readerPath = path / "reader";
        std::filesystem::create_directory(readerPath);
        std::filesystem::permissions(readerPath, std::filesystem::perms::owner_all);
        FileRetainedPosition checkpoint{readerPath, "witness"};
        RetainedPosition     position;
        position.raiseHead("witness", 1);
        position.raiseAnchor("consumer/boot", {.position = 1, .digest = chainInitialValue, .counter = 1});
        if (!checkpoint.initialize(position))
            result = 5;
        const auto restored = checkpoint.load();
        if (!restored || restored->position.allAnchors() != position.allAnchors())
            result = 6;
    }
    if (result == 0) {
        const auto witnessPath = path / "witness";
        std::filesystem::create_directory(witnessPath);
        std::filesystem::permissions(witnessPath, std::filesystem::perms::owner_all);
        if (!FileAnchorAuthority::initialize({.directory = witnessPath, .providerId = "witness"}))
            result = 7;
        auto authority = FileAnchorAuthority::open({.directory = witnessPath, .providerId = "witness"});
        if (!authority || !std::holds_alternative<AnchorStamp>((*authority)->advance(makeAnchorClaim("consumer/boot", 1, chainInitialValue))))
            result = 8;
        UnixAnchorProvider unavailable{
            {.socketPath = path / "absent", .providerId = "witness", .serverUid = 0}
        };
        if (!std::holds_alternative<ProviderUnavailable>(unavailable.streams()))
            result = 9;
    }
    std::filesystem::remove_all(path);
    return result;
}
