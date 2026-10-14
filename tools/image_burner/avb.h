// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "common.h"
#include <libavb/libavb.h>

namespace kemu {
struct TreeDescriptor {
    size_t offset;
    AvbHashtreeDescriptor header;
    std::string name;
    std::string algorithm;
    Bytes salt;
    Bytes root;
};
struct ChainDescriptor { std::string name; Bytes key; };
struct PartitionAvb { Bytes vbmeta; uint64_t data_size; };
Bytes ReadVbmeta(int fd, uint64_t offset, uint64_t available);
PartitionAvb ReadPartitionAvb(const std::string& image);
AvbVBMetaImageHeader Header(const Bytes& vbmeta);
Bytes PublicKey(const Bytes& vbmeta);
std::vector<TreeDescriptor> Trees(const Bytes& vbmeta);
std::vector<ChainDescriptor> Chains(const Bytes& vbmeta);
void VerifySignature(const Bytes& vbmeta, bool allow_unsigned = false);
void CheckSigningKey(const Bytes& vbmeta, const std::string& pem);
void VerifyTree(const std::string& image, const TreeDescriptor& tree);
void Sign(Bytes& vbmeta, const std::string& pem);
Bytes SealPartition(const std::string& image, Bytes original_vbmeta, const std::string& key);
void ReplaceTree(Bytes& top, const TreeDescriptor& descriptor, const Bytes& partition_vbmeta);
std::pair<std::string, uint64_t> VbmetaDigest(const Bytes& top,
                                           const std::map<std::string, std::string>& partitions);
}  // namespace kemu
