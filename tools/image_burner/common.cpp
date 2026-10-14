// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#include "common.h"
#include <openssl/evp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace kemu {
void Check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
uint64_t Align(uint64_t value, uint64_t alignment) {
    Check(alignment && value <= UINT64_MAX - alignment, "alignment overflow");
    return (value + alignment - 1) / alignment * alignment;
}
void Bounds(uint64_t offset, uint64_t length, uint64_t size) {
    Check(offset <= size && length <= size - offset, "image range out of bounds");
}
Fd OpenRead(const std::string& path) {
    Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    Check(fd.ok(), "cannot open regular input: " + path + ": " + strerror(errno));
    struct stat st{};
    Check(fstat(fd, &st) == 0 && S_ISREG(st.st_mode), "input must be a regular file: " + path);
    return fd;
}
Fd OpenWrite(const std::string& path, bool exclusive) {
    Fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW |
                           (exclusive ? O_CREAT | O_EXCL : 0), 0600));
    Check(fd.ok(), "cannot open output: " + path + ": " + strerror(errno));
    struct stat st{};
    Check(fstat(fd, &st) == 0 && S_ISREG(st.st_mode), "output must be a regular file");
    return fd;
}
uint64_t Size(int fd) {
    struct stat st{};
    Check(fstat(fd, &st) == 0 && st.st_size >= 0, "fstat failed");
    return st.st_size;
}
uint64_t FileSize(const std::string& path) { return Size(OpenRead(path)); }
Bytes Read(int fd, uint64_t offset, size_t length) {
    Bounds(offset, length, Size(fd));
    Bytes out(length);
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, out.data() + done, length - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        Check(n > 0, "short image read");
        done += n;
    }
    return out;
}
void Write(int fd, uint64_t offset, const Bytes& bytes) {
    Check(offset <= INT64_MAX && bytes.size() <= INT64_MAX - offset, "write overflow");
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = pwrite(fd, bytes.data() + done, bytes.size() - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        Check(n > 0, "image write failed: " + std::string(strerror(errno)));
        done += n;
    }
}
void CopyRange(int in, int out, uint64_t src, uint64_t dst, uint64_t length) {
    Bounds(src, length, Size(in));
    for (uint64_t done = 0; done < length;) {
        size_t n = std::min<uint64_t>(4 * kMiB, length - done);
        auto data = Read(in, src + done, n);
        // Preserve sparse regions without depending on SEEK_HOLE support.
        if (std::any_of(data.begin(), data.end(), [](uint8_t b) { return b != 0; }))
            Write(out, dst + done, data);
        else if (dst + done < Size(out))
            Write(out, dst + done, data);
        done += n;
    }
    if (Size(out) < dst + length) Check(ftruncate(out, dst + length) == 0, "truncate failed");
}
void Resize(const std::string& path, uint64_t length) {
    Check(length <= INT64_MAX, "image too large");
    Check(ftruncate(OpenWrite(path), length) == 0, "cannot resize image");
}
void Sync(const std::string& path) { Check(fsync(OpenWrite(path)) == 0, "fsync failed"); }
std::string Hex(const Bytes& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
Bytes Digest(const std::string& algorithm, const uint8_t* data, size_t size, const Bytes& prefix) {
    const EVP_MD* md = EVP_get_digestbyname(algorithm.c_str());
    Check(md != nullptr, "unsupported hash algorithm: " + algorithm);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    Check(ctx && EVP_DigestInit_ex(ctx.get(), md, nullptr) == 1, "hash init failed");
    Check(EVP_DigestUpdate(ctx.get(), prefix.data(), prefix.size()) == 1 &&
          EVP_DigestUpdate(ctx.get(), data, size) == 1, "hash update failed");
    Bytes out(EVP_MD_size(md)); unsigned length = 0;
    Check(EVP_DigestFinal_ex(ctx.get(), out.data(), &length) == 1, "hash finish failed");
    out.resize(length); return out;
}
std::string Capture(const std::vector<std::string>& args, int input) {
    Check(!args.empty(), "empty command");
    int ends[2]; Check(pipe2(ends, O_CLOEXEC) == 0, "pipe failed");
    Fd reader(ends[0]), writer(ends[1]);
    pid_t pid = fork(); Check(pid >= 0, "fork failed");
    if (pid == 0) {
        if (dup2(writer, STDOUT_FILENO) < 0 || (input >= 0 && dup2(input, STDIN_FILENO) < 0)) _exit(126);
        std::vector<char*> argv; for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr); execv(argv[0], argv.data()); _exit(127);
    }
    writer.reset(); std::string output; bool valid = true; char buffer[4096];
    for (;;) {
        auto n = read(reader, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { valid = false; break; }
        if (!n) break;
        if (output.size() + n <= 65536) output.append(buffer, n); else valid = false;
    }
    reader.reset(); int status; pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "command failed: " + args[0]);
    Check(valid, "invalid or oversized command output: " + args[0]); return output;
}
std::string FileHash(const std::string& path) {
    auto fd = OpenRead(path);
#ifdef __ANDROID__
    const char* executable = "/system/bin/sha256sum";
#else
    const char* executable = "/usr/bin/sha256sum";
#endif
    auto result = Capture({executable}, fd);
    Check(result.size() >= 65 && result.substr(0, 64).find_first_not_of("0123456789abcdef") == std::string::npos &&
          (result.substr(64) == "  -\n" || result.substr(64) == "  -\r\n"), "invalid sha256sum output");
    return result.substr(0, 64);
}
void SaveText(const std::string& path, const std::string& data) {
    auto fd = OpenWrite(path, true); Write(fd, 0, Bytes(data.begin(), data.end()));
    Check(fsync(fd) == 0, "fsync text failed");
}
Json::Value LoadJson(const std::string& path) {
    auto fd = OpenRead(path); Check(Size(fd) <= 4 * kMiB, "manifest too large");
    auto bytes = Read(fd, 0, Size(fd));
    Json::CharReaderBuilder builder; builder["rejectDupKeys"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value result; std::string error;
    const char* start = reinterpret_cast<const char*>(bytes.data());
    Check(reader->parse(start, start + bytes.size(), &result, &error), "invalid manifest: " + error);
    return result;
}
void SaveJson(const std::string& path, const Json::Value& value) {
    Json::StreamWriterBuilder builder; builder["indentation"] = "  ";
    SaveText(path, Json::writeString(builder, value) + "\n");
}
int Run(const std::vector<std::string>& args) {
    Check(!args.empty(), "empty command");
    pid_t pid = fork(); Check(pid >= 0, "fork failed");
    if (pid == 0) {
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr); execv(argv[0], argv.data());
        perror(argv[0]); _exit(127);
    }
    int status; while (waitpid(pid, &status, 0) < 0) Check(errno == EINTR, "waitpid failed");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
void Fsck(const std::string& executable, const std::string& image, bool unshare) {
    std::vector<std::string> args{executable, "-f", "-y"};
    if (unshare) { args.push_back("-E"); args.push_back("unshare_blocks"); }
    args.push_back(image);
    int code = Run(args);
    Check(code == 0 || code == 1 || code == 2, "e2fsck failed with status " + std::to_string(code));
}
std::string SafeRelative(const std::string& path) {
    Check(!path.empty() && path.front() != '/' && path.back() != '/', "invalid relative path");
    size_t begin = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size()) {
            unsigned char c = path[i]; Check(c >= 32 && c < 127 && c != '\\', "invalid path character");
        }
        if (i == path.size() || path[i] == '/') {
            auto part = path.substr(begin, i - begin);
            Check(!part.empty() && part != "." && part != "..", "unsafe path component");
            begin = i + 1;
        }
    }
    return path;
}
std::string PayloadPath(const std::string& root, const std::string& relative) {
    auto base = std::filesystem::canonical(root).string();
    auto path = std::filesystem::canonical(base + "/" + SafeRelative(relative)).string();
    Check(path.compare(0, base.size() + 1, base + "/") == 0, "payload escapes its directory");
    return path;
}
void Progress(const std::string& message) { std::cout << message << std::endl; }
Mapping::Mapping(const std::string& path, uint64_t length) : fd(OpenRead(path)), size(length) {
    Bounds(0, size, Size(fd)); Check(size != 0, "empty mapping");
    void* ptr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    Check(ptr != MAP_FAILED, "mmap failed"); data = static_cast<uint8_t*>(ptr);
}
Mapping::~Mapping() { if (data) munmap(const_cast<uint8_t*>(data), size); }
}  // namespace kemu
