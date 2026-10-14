// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: MIT
#pragma once
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace kemu {
using Bytes = std::vector<uint8_t>;
constexpr uint64_t kMiB = 1024 * 1024;
inline void Check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
class Fd {
    int fd_;
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    operator int() const { return fd_; }
    bool ok() const { return fd_ >= 0; }
    int release() { int fd = fd_; fd_ = -1; return fd; }
};
inline Fd OpenRead(const std::string& path) {
    Fd fd(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat st{};
    Check(fd.ok() && fstat(fd, &st) == 0 && S_ISREG(st.st_mode), "input must be a regular file: " + path);
    return fd;
}
inline uint64_t Size(int fd) { struct stat st{}; Check(fstat(fd, &st) == 0 && st.st_size >= 0, "fstat failed"); return st.st_size; }
inline uint64_t FileSize(const std::string& path) { return Size(OpenRead(path)); }
inline uint64_t Align(uint64_t n, uint64_t a) { Check(a && n <= UINT64_MAX - a, "size overflow"); return (n + a - 1) / a * a; }
inline Bytes Read(int fd, uint64_t offset, size_t count) {
    auto size = Size(fd); Check(offset <= size && count <= size - offset, "read out of bounds");
    Bytes bytes(count); size_t done = 0;
    while (done < count) { auto n = pread(fd, bytes.data() + done, count - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        Check(n > 0, "short read"); done += n;
    }
    return bytes;
}
inline std::string SafeRelative(const std::string& path) {
    Check(!path.empty() && path.front() != '/' && path.back() != '/', "invalid path");
    size_t begin = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i < path.size()) { auto c = static_cast<unsigned char>(path[i]); Check(c >= 32 && c < 127 && c != '\\', "invalid path character"); }
        if (i == path.size() || path[i] == '/') { auto part = path.substr(begin, i - begin); Check(!part.empty() && part != "." && part != "..", "unsafe path"); begin = i + 1; }
    }
    return path;
}
}
