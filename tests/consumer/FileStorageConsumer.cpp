/** @brief Direct-import consumer of the optional file backend, in source and installed packages. */
#include <cstdlib>

import std;
import mddlog.adapter.filestoragemedium;

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
    std::filesystem::remove_all(path);
    return result;
}
