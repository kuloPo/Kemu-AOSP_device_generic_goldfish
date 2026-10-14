// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#include "filesystem.h"
#include <filesystem>
namespace kemu {
namespace { std::string helper; }
void SetFilesystemTool(const std::string& path) { helper = path; }
uint64_t WorkingFilesystemSize(const std::string& image, const std::vector<Replacement>& files) {
    uint64_t extra = 0;
    for (const auto& file : files) extra += Align(FileSize(file.source), 4096);
    auto text = Capture({helper, "size", image, std::to_string(extra)});
    Check(!text.empty() && text.back() == '\n', "invalid ext4 size response"); text.pop_back();
    Check(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos, "invalid ext4 size response");
    auto size = std::stoull(text);
    Check(size && size <= 32ULL * 1024 * kMiB && size % 4096 == 0, "invalid ext4 working size");
    return size;
}
void ReplaceFiles(const std::string& image, const std::string& partition, const std::vector<Replacement>& files) {
    for (const auto& file : files) {
        auto path = partition == "system" ? file.path : file.path.substr(partition.size() + 1);
        Check(Run({helper, "replace", image, path, file.source}) == 0, "ext4 replacement failed: " + file.path);
        auto extracted = image + ".readback";
        Check(Run({helper, "extract", image, path, extracted}) == 0, "ext4 extraction failed: " + file.path);
        Check(FileHash(extracted) == file.sha256, "replacement readback hash mismatch: " + file.path);
        Check(std::filesystem::remove(extracted), "cannot remove readback file");
        Progress("Replaced " + file.path);
    }
}
}
