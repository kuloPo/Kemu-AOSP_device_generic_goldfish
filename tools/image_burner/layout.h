// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "common.h"
#include <liblp/liblp.h>

namespace kemu {
struct Gpt {
    Bytes mbr, header, entries;
    size_t super_index, vbmeta_index;
    uint64_t super_offset, super_size, vbmeta_offset, vbmeta_size;
};
Gpt ReadGpt(const std::string& image);
std::unique_ptr<android::fs_mgr::LpMetadata> ExtractPartitions(
    const std::string& image, const Gpt& gpt, const std::string& work,
    std::map<std::string, std::string>& partitions);
void PackSuper(const android::fs_mgr::LpMetadata& original,
               const std::map<std::string, std::string>& partitions, const std::string& output);
void PackDisk(const Gpt& original, const Bytes& vbmeta, const std::string& super,
              const std::string& output);
void VerifyPacked(const std::string& image, const Bytes& vbmeta,
                  const std::map<std::string, std::string>& partitions, const std::string& scratch);
}  // namespace kemu
