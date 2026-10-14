// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#include "layout.h"
#include <liblp/builder.h>
#include <zlib.h>
#include <unistd.h>
#include <endian.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace kemu {
namespace lp = android::fs_mgr;
namespace {
uint32_t U32(const Bytes& b, size_t offset) {
    Bounds(offset, 4, b.size()); uint32_t x; memcpy(&x, b.data() + offset, 4); return le32toh(x);
}
uint64_t U64(const Bytes& b, size_t offset) {
    Bounds(offset, 8, b.size()); uint64_t x; memcpy(&x, b.data() + offset, 8); return le64toh(x);
}
void Put32(Bytes& b, size_t offset, uint32_t value) {
    Bounds(offset, 4, b.size()); value = htole32(value); memcpy(b.data() + offset, &value, 4);
}
void Put64(Bytes& b, size_t offset, uint64_t value) {
    Bounds(offset, 8, b.size()); value = htole64(value); memcpy(b.data() + offset, &value, 8);
}
uint32_t Crc(const Bytes& bytes) { return crc32(0, bytes.data(), bytes.size()); }
void HeaderCrc(Bytes& header) {
    Put32(header, 16, 0); Put32(header, 16, Crc(Bytes(header.begin(), header.begin() + U32(header, 12))));
}
Bytes ReadHeader(int fd, uint64_t sector) {
    auto h = Read(fd, sector * 512, 512);
    Check(memcmp(h.data(), "EFI PART", 8) == 0 && U32(h, 8) == 0x10000,
          "invalid GPT signature/version");
    uint32_t length = U32(h, 12); Check(length >= 92 && length <= 512, "invalid GPT header length");
    uint32_t expected = U32(h, 16); auto copy = h; HeaderCrc(copy);
    Check(U32(copy, 16) == expected && U64(h, 24) == sector, "GPT header CRC/location mismatch");
    return h;
}
}  // namespace

Gpt ReadGpt(const std::string& image) {
    auto fd = OpenRead(image); uint64_t size = Size(fd);
    Check(size >= 34 * 512 && size % 512 == 0 && size <= 128ULL * 1024 * kMiB,
          "unsupported disk image size");
    Gpt g{}; g.mbr = Read(fd, 0, 512); g.header = ReadHeader(fd, 1);
    Check(g.mbr[510] == 0x55 && g.mbr[511] == 0xaa, "missing protective MBR");
    auto backup = ReadHeader(fd, size / 512 - 1);
    Check(U64(g.header, 32) == size / 512 - 1 && U64(backup, 32) == 1, "GPT backup mismatch");
    Check(std::equal(g.header.begin() + 56, g.header.begin() + 72, backup.begin() + 56), "GPT GUID mismatch");
    uint32_t count = U32(g.header, 80), entry_size = U32(g.header, 84);
    Check(count > 0 && count <= 1024 && entry_size == 128 && U64(g.header, 72) == 2,
          "unsupported GPT entry table");
    g.entries = Read(fd, U64(g.header, 72) * 512, uint64_t(count) * entry_size);
    Check(Crc(g.entries) == U32(g.header, 88), "GPT entry CRC mismatch");
    Check(U32(backup, 80) == count && U32(backup, 84) == entry_size && U32(backup, 88) == Crc(g.entries),
          "backup GPT table layout mismatch");
    Check(Read(fd, U64(backup, 72) * 512, g.entries.size()) == g.entries, "backup GPT entries mismatch");
    std::map<std::string, size_t> names;
    for (size_t i = 0; i < count; ++i) {
        size_t base = i * entry_size;
        if (std::all_of(g.entries.begin() + base, g.entries.begin() + base + 16,
                        [](uint8_t b) { return b == 0; })) continue;
        std::string name;
        for (size_t n = 56; n < 128; n += 2) {
            uint8_t ch = g.entries[base + n]; Check(g.entries[base + n + 1] == 0, "non-ASCII GPT name");
            if (!ch) break;
            name += ch;
        }
        Check(!names.count(name), "duplicate GPT partition name"); names[name] = i;
        uint64_t start = U64(g.entries, base + 32), end = U64(g.entries, base + 40);
        Check(start >= U64(g.header, 40) && end >= start && end <= U64(g.header, 48), "invalid GPT partition bounds");
        Bounds(start * 512, (end - start + 1) * 512, size);
    }
    Check(names.size() == 2 && names.count("super") && names.count("vbmeta"),
          "expected Kemu GPT with exactly vbmeta and super partitions");
    g.super_index = names.at("super"); g.vbmeta_index = names.at("vbmeta");
    auto range = [&](size_t i) {
        uint64_t start = U64(g.entries, i * 128 + 32), end = U64(g.entries, i * 128 + 40);
        return std::pair<uint64_t,uint64_t>{start * 512, (end - start + 1) * 512};
    };
    std::tie(g.super_offset, g.super_size) = range(g.super_index);
    std::tie(g.vbmeta_offset, g.vbmeta_size) = range(g.vbmeta_index);
    Check(g.vbmeta_offset + g.vbmeta_size <= g.super_offset && g.super_offset % kMiB == 0,
          "unsupported physical partition order/alignment");
    return g;
}
std::unique_ptr<lp::LpMetadata> ExtractPartitions(const std::string& image, const Gpt& g,
                                                const std::string& work,
                                                std::map<std::string, std::string>& partitions) {
    Check(std::filesystem::create_directory(work), "partition work directory already exists");
    std::string raw = work + "/super.input.img";
    { auto in = OpenRead(image); auto out = OpenWrite(raw, true);
      CopyRange(in, out, g.super_offset, 0, g.super_size); }
    auto meta = lp::ReadMetadata(raw, 0);
    Check(meta != nullptr && meta->block_devices.size() == 1 && meta->header.flags == 0,
          "unsupported super metadata (requires single-device, non-Virtual-A/B layout)");
    Check(lp::GetBlockDevicePartitionName(meta->block_devices[0]) == "super" &&
          meta->block_devices[0].flags == 0 && meta->block_devices[0].size == g.super_size,
          "super block device mismatch");
    auto source = OpenRead(raw);
    for (const auto& p : meta->partitions) {
        std::string name = lp::GetPartitionName(p);
        Check(SafeRelative(name).find('/') == std::string::npos && !partitions.count(name), "unsafe partition name");
        Check((p.attributes & ~LP_PARTITION_ATTR_READONLY) == 0, "slotted/updated partition is not supported");
        Bounds(p.first_extent_index, p.num_extents, meta->extents.size());
        std::string target = work + "/" + name + ".img";
        auto out = OpenWrite(target, true); uint64_t offset = 0;
        for (uint32_t i = 0; i < p.num_extents; ++i) {
            const auto& e = meta->extents[p.first_extent_index + i];
            Check(e.num_sectors <= g.super_size / 512, "logical extent too large");
            uint64_t length = e.num_sectors * 512;
            if (e.target_type == LP_TARGET_TYPE_LINEAR) {
                Check(e.target_source == 0 && e.target_data <= g.super_size / 512, "invalid extent device/offset");
                CopyRange(source, out, e.target_data * 512, offset, length);
            } else {
                Check(e.target_type == LP_TARGET_TYPE_ZERO, "unsupported extent type");
                Check(ftruncate(out, offset + length) == 0, "cannot allocate zero extent");
            }
            offset += length;
        }
        Check(offset == lp::GetPartitionSize(*meta, p), "logical partition size mismatch");
        partitions[name] = target;
    }
    Check(std::filesystem::remove(raw), "cannot remove temporary super copy");
    return meta;
}
void PackSuper(const lp::LpMetadata& original, const std::map<std::string, std::string>& partitions,
               const std::string& output) {
    uint64_t total = 4 * kMiB;
    std::map<std::string, uint64_t> group_sizes;
    for (const auto& p : original.partitions) {
        Check(p.group_index < original.groups.size(), "invalid partition group");
        uint64_t size = FileSize(partitions.at(lp::GetPartitionName(p)));
        total += Align(size, kMiB);
        group_sizes[lp::GetPartitionGroupName(original.groups[p.group_index])] += size;
    }
    uint64_t super_size = Align(std::max(original.block_devices[0].size, total + 16 * kMiB), kMiB);
    Check(super_size <= 128ULL * 1024 * kMiB, "rebuilt super exceeds supported size");
    lp::BlockDeviceInfo info("super", super_size, kMiB, 0, original.geometry.logical_block_size);
    auto builder = lp::MetadataBuilder::New(info, original.geometry.metadata_max_size,
                                           original.geometry.metadata_slot_count);
    Check(builder != nullptr, "cannot create super metadata");
    for (const auto& group : original.groups) {
        Check(group.flags == 0, "slotted groups are not supported");
        auto name = lp::GetPartitionGroupName(group);
        if (name == "default") continue;
        uint64_t maximum = group.maximum_size ? std::max(group.maximum_size, Align(group_sizes[name], kMiB)) : 0;
        Check(builder->AddGroup(name, maximum), "cannot create group: " + name);
    }
    for (const auto& p : original.partitions) {
        auto name = lp::GetPartitionName(p);
        auto* partition = builder->AddPartition(name, lp::GetPartitionGroupName(original.groups[p.group_index]), p.attributes);
        Check(partition && builder->ResizePartition(partition, FileSize(partitions.at(name))),
              "cannot allocate logical partition: " + name);
    }
    auto meta = builder->Export(); Check(meta != nullptr, "cannot export super metadata");
    Check(!std::filesystem::exists(output), "super output already exists");
    Check(lp::WriteToImageFile(output, *meta, 4096, partitions, false), "super image assembly failed");
    Sync(output);
}
void PackDisk(const Gpt& g, const Bytes& vbmeta, const std::string& super, const std::string& output) {
    Check(vbmeta.size() <= g.vbmeta_size, "VBMeta no longer fits physical partition");
    uint64_t super_size = FileSize(super);
    uint64_t disk_size = Align(g.super_offset + super_size + Align(g.entries.size(), 512) + 512, kMiB);
    uint64_t last = disk_size / 512 - 1, backup_entries = last - Align(g.entries.size(), 512) / 512;
    auto entries = g.entries;
    Put64(entries, g.super_index * 128 + 40, (g.super_offset + super_size) / 512 - 1);
    auto primary = g.header;
    Put64(primary, 32, last); Put64(primary, 48, backup_entries - 1); Put32(primary, 88, Crc(entries));
    HeaderCrc(primary);
    auto backup = primary;
    Put64(backup, 24, last); Put64(backup, 32, 1); Put64(backup, 72, backup_entries); HeaderCrc(backup);
    auto mbr = g.mbr;
    bool protective = false;
    for (size_t p = 446; p < 510; p += 16) {
        Check(mbr[p + 4] == 0 || mbr[p + 4] == 0xee, "hybrid MBR is not supported");
        if (mbr[p + 4] == 0xee) { Put32(mbr, p + 12, std::min<uint64_t>(last, UINT32_MAX)); protective = true; }
    }
    Check(protective, "protective MBR entry missing");
    auto out = OpenWrite(output, true);
    Write(out, 0, mbr); Write(out, 512, primary); Write(out, 1024, entries);
    Write(out, g.vbmeta_offset, vbmeta);
    auto in = OpenRead(super); CopyRange(in, out, 0, g.super_offset, super_size);
    Write(out, backup_entries * 512, entries); Write(out, last * 512, backup);
    Check(ftruncate(out, disk_size) == 0 && fsync(out) == 0, "cannot finish disk image");
    ReadGpt(output);
}
void VerifyPacked(const std::string& image, const Bytes& vbmeta,
                  const std::map<std::string, std::string>& partitions, const std::string& scratch) {
    auto gpt = ReadGpt(image); auto fd = OpenRead(image);
    Check(Read(fd, gpt.vbmeta_offset, vbmeta.size()) == vbmeta, "packed VBMeta mismatch");
    std::map<std::string, std::string> unpacked;
    ExtractPartitions(image, gpt, scratch, unpacked);
    Check(unpacked.size() == partitions.size(), "packed partition count mismatch");
    for (const auto& [name, path] : partitions) {
        Check(unpacked.count(name) && FileHash(unpacked.at(name)) == FileHash(path),
              "packed partition verification failed: " + name);
        Check(std::filesystem::remove(unpacked.at(name)), "cannot remove verification copy");
    }
    Check(std::filesystem::remove(scratch), "cannot remove verification directory");
}
}  // namespace kemu
