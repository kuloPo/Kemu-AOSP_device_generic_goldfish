// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <android-base/unique_fd.h>
#include <json/json.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kemu {
using Bytes = std::vector<uint8_t>;
using Fd = android::base::unique_fd;
constexpr uint64_t kMiB = 1024 * 1024;
void Check(bool ok, const std::string& message);
uint64_t Align(uint64_t value, uint64_t alignment);
void Bounds(uint64_t offset, uint64_t length, uint64_t size);
Fd OpenRead(const std::string& path);
Fd OpenWrite(const std::string& path, bool exclusive = false);
uint64_t Size(int fd);
uint64_t FileSize(const std::string& path);
Bytes Read(int fd, uint64_t offset, size_t length);
void Write(int fd, uint64_t offset, const Bytes& bytes);
void CopyRange(int in, int out, uint64_t src, uint64_t dst, uint64_t length);
void Resize(const std::string& path, uint64_t length);
void Sync(const std::string& path);
std::string Hex(const Bytes& bytes);
Bytes Digest(const std::string& algorithm, const uint8_t* data, size_t size,
             const Bytes& prefix = {});
std::string FileHash(const std::string& path);
void SaveText(const std::string& path, const std::string& data);
Json::Value LoadJson(const std::string& path);
void SaveJson(const std::string& path, const Json::Value& value);
std::string Capture(const std::vector<std::string>& args, int input = -1);
int Run(const std::vector<std::string>& args);
void Fsck(const std::string& executable, const std::string& image, bool unshare = false);
std::string SafeRelative(const std::string& path);
std::string PayloadPath(const std::string& root, const std::string& relative);
void Progress(const std::string& message);

struct Mapping {
    explicit Mapping(const std::string& path, uint64_t length);
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Fd fd;
    const uint8_t* data = nullptr;
    uint64_t size = 0;
};
}  // namespace kemu
