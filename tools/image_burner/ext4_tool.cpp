// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: MIT
#include "ext4_support.h"
#include <cstring>
#include <memory>
extern "C" {
#include <ext2fs/ext2fs.h>
}

namespace kemu {
namespace {
void E(errcode_t code, const std::string& operation) {
    Check(code == 0, operation + " failed (ext2fs error " + std::to_string(code) + ")");
}
struct Ext {
    explicit Ext(const std::string& image, bool write) {
        E(ext2fs_open(image.c_str(), EXT2_FLAG_64BITS | (write ? EXT2_FLAG_RW : 0),
                      0, 0, unix_io_manager, &fs), "open ext4");
        Check(fs->blocksize == 4096, "only 4096-byte ext4 blocks are supported");
        if (write) {
            Check(!ext2fs_has_feature_shared_blocks(fs->super), "shared blocks must be unshared before editing");
            E(ext2fs_read_bitmaps(fs), "read ext4 bitmaps");
        }
    }
    ~Ext() { if (fs) ext2fs_close(fs); }
    void Close() { auto value = fs; fs = nullptr; E(ext2fs_close(value), "close ext4"); }
    ext2_filsys fs = nullptr;
};
ext2_ino_t Resolve(ext2_filsys fs, const std::string& path) {
    Check(!path.empty() && path.front() == '/', "path must be absolute");
    std::string relative = SafeRelative(path.substr(1));
    ext2_ino_t inode = EXT2_ROOT_INO;
    size_t begin = 0;
    while (begin < relative.size()) {
        size_t end = relative.find('/', begin); if (end == std::string::npos) end = relative.size();
        auto name = relative.substr(begin, end - begin); ext2_ino_t next;
        E(ext2fs_lookup(fs, inode, name.c_str(), name.size(), nullptr, &next), "lookup " + path);
        ext2_inode info{}; E(ext2fs_read_inode(fs, next, &info), "read inode");
        Check(!LINUX_S_ISLNK(info.i_mode), "symlink path is not a replacement target: " + path);
        if (end != relative.size()) Check(LINUX_S_ISDIR(info.i_mode), "non-directory path component");
        inode = next; begin = end + 1;
    }
    return inode;
}
}  // namespace

uint64_t WorkingFilesystemSize(const std::string& image) {
    auto regular = OpenRead(image);
    Ext ext(image, false);
    uint64_t logical = 0;
    ext2_inode_scan scan;
    E(ext2fs_open_inode_scan(ext.fs, 0, &scan), "scan ext4 inodes");
    for (;;) {
        ext2_ino_t number; ext2_inode inode{};
        E(ext2fs_get_next_inode(scan, &number, &inode), "scan inode");
        if (!number) break;
        if (inode.i_links_count && (LINUX_S_ISREG(inode.i_mode) || LINUX_S_ISDIR(inode.i_mode))) {
            uint64_t size = EXT2_I_SIZE(&inode);
            Check(size < 32ULL * 1024 * kMiB && logical < 32ULL * 1024 * kMiB - Align(size, 4096),
                  "filesystem logical data exceeds 32 GiB");
            logical += Align(size, 4096);
        }
    }
    ext2fs_close_inode_scan(scan);
    // Enough room for full unsharing, filesystem metadata, replacement growth and AVB-independent slack.
    uint64_t target = Align(std::max(FileSize(image), logical + logical / 4 + 64 * kMiB), kMiB);
    Check(target <= 32ULL * 1024 * kMiB, "working filesystem exceeds 32 GiB");
    return target;
}
void ReplaceFile(const std::string& image, const std::string& path,
                 const std::string& source_path) {
    auto regular = OpenRead(image);
    Ext ext(image, true);
    auto inode = Resolve(ext.fs, path);
    ext2_inode before{}; E(ext2fs_read_inode(ext.fs, inode, &before), "read target inode");
    Check(LINUX_S_ISREG(before.i_mode) && before.i_links_count == 1,
          "replacement must be an existing regular file with one link: " + path);
    auto source = OpenRead(source_path);
    ext2_file_t target; E(ext2fs_file_open(ext.fs, inode, EXT2_FILE_WRITE, &target), "open replacement target");
    // Retaining the inode preserves ownership, mode, timestamps and xattrs (including SELinux).
    E(ext2fs_file_set_size2(target, 0), "truncate target file");
    for (uint64_t pos = 0; pos < Size(source);) {
        auto buffer = Read(source, pos, std::min<uint64_t>(kMiB, Size(source) - pos));
        unsigned written = 0;
        E(ext2fs_file_write(target, buffer.data(), buffer.size(), &written), "write replacement");
        Check(written == buffer.size(), "short ext4 write"); pos += written;
    }
    E(ext2fs_file_close(target), "close replacement");
    E(ext2fs_flush(ext.fs), "flush ext4");
    ext2_inode after{}; E(ext2fs_read_inode(ext.fs, inode, &after), "read updated inode");
    Check(before.i_mode == after.i_mode && before.i_uid == after.i_uid && before.i_gid == after.i_gid &&
          before.i_file_acl == after.i_file_acl && before.i_links_count == after.i_links_count,
          "target metadata unexpectedly changed");
    ext.Close();
}
}  // namespace kemu

int main(int argc, char** argv) {
    try {
        using namespace kemu;
        Check(argc >= 3, "usage: kemu_ext4_tool size IMAGE PAYLOAD_BYTES | replace IMAGE PATH SOURCE | extract IMAGE PATH OUTPUT");
        std::string command = argv[1], image = argv[2];
        if (command == "size" && argc == 4) {
            // Supply the total payload bytes through a separate size argument.
            size_t used = 0; auto extra = std::stoull(argv[3], &used);
            Check(used == strlen(argv[3]) && extra <= 32ULL * 1024 * kMiB, "invalid payload size");
            auto size = WorkingFilesystemSize(image);
            auto target = Align(size + extra + extra / 4, kMiB);
            Check(target <= 32ULL * 1024 * kMiB, "working filesystem exceeds 32 GiB");
            std::cout << target << '\n';
        } else if (command == "replace" && argc == 5) {
            ReplaceFile(image, argv[3], argv[4]);
        } else if (command == "extract" && argc == 5) {
            auto regular = OpenRead(image);
            Ext ext(image, false); auto inode = Resolve(ext.fs, argv[3]);
            ext2_inode info{}; E(ext2fs_read_inode(ext.fs, inode, &info), "read inode");
            Check(LINUX_S_ISREG(info.i_mode), "extract target is not a regular file");
            auto output = Fd(open(argv[4], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            Check(output.ok(), "cannot create extraction output");
            ext2_file_t file; E(ext2fs_file_open(ext.fs, inode, 0, &file), "open ext4 file");
            Bytes buffer(kMiB);
            for (;;) {
                unsigned got = 0; E(ext2fs_file_read(file, buffer.data(), buffer.size(), &got), "read ext4 file");
                if (!got) break;
                size_t done = 0;
                while (done < got) {
                    auto n = write(output, buffer.data() + done, got - done);
                    if (n < 0 && errno == EINTR) continue;
                    Check(n > 0, "extraction write failed"); done += n;
                }
            }
            E(ext2fs_file_close(file), "close extracted file");
            Check(fsync(output) == 0, "extraction sync failed");
            int fd = output.release(); Check(close(fd) == 0, "extraction close failed");
            ext.Close();
        } else Check(false, "unknown command or invalid arguments");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "kemu_ext4_tool: " << e.what() << '\n'; return 1;
    }
}
