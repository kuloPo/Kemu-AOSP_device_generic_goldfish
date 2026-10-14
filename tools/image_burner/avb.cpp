// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#include "avb.h"
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <thread>
#include <unistd.h>
extern "C" {
#include <fec.h>
}

namespace kemu {
namespace {
struct TreeData { Bytes root; Bytes bytes; };
TreeData BuildTree(const std::string& image, uint64_t size, const TreeDescriptor& spec) {
    Check(spec.header.data_block_size == 4096 && spec.header.hash_block_size == 4096,
          "only 4096-byte AVB blocks are supported");
    Check(size && size % 4096 == 0, "unaligned verity data");
    Check(spec.algorithm == "sha256" || spec.algorithm == "sha1", "unsupported tree hash");
    Mapping input(image, size);
    const uint8_t* source = input.data;
    uint64_t length = size;
    // sha1 digests occupy 32-byte slots; sha256 digests are also 32 bytes.
    std::vector<Bytes> levels;
    while (length > 4096) {
        Bytes level(Align((length / 4096) * 32, 4096), 0);
        for (uint64_t i = 0; i < length / 4096; ++i) {
            auto digest = Digest(spec.algorithm, source + i * 4096, 4096, spec.salt);
            std::copy(digest.begin(), digest.end(), level.begin() + i * 32);
        }
        levels.push_back(std::move(level));
        source = levels.back().data(); length = levels.back().size();
    }
    TreeData result{Digest(spec.algorithm, source, length, spec.salt), {}};
    for (auto it = levels.rbegin(); it != levels.rend(); ++it)
        result.bytes.insert(result.bytes.end(), it->begin(), it->end());
    return result;
}
Bytes EncodeFec(const std::string& image, uint64_t length, uint32_t roots) {
    if (roots == 0) return {};
    Check(roots > 0 && roots < 255 && length % 4096 == 0, "invalid FEC parameters");
    Mapping input(image, length);
    uint64_t rsn = 255 - roots;
    uint64_t rounds = (length / 4096 + rsn - 1) / rsn;
    uint64_t words = rounds * 4096;
    Bytes parity(words * roots);
    unsigned threads = std::min<uint64_t>(8, std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    std::vector<void*> encoders;
    for (unsigned t = 0; t < threads; ++t) {
        void* encoder = init_rs_char(8, 0x11d, 0, 1, roots, 0);
        Check(encoder != nullptr, "FEC encoder initialization failed"); encoders.push_back(encoder);
    }
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            uint8_t word[255];
            for (uint64_t i = words * t / threads; i < words * (t + 1) / threads; ++i) {
                for (uint64_t j = 0; j < rsn; ++j) {
                    uint64_t offset = i + j * rounds * 4096;
                    word[j] = offset < length ? input.data[offset] : 0;
                }
                encode_rs_char(encoders[t], word, parity.data() + i * roots);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (void* encoder : encoders) free_rs_char(encoder);
    return parity;
}
std::vector<std::pair<size_t, size_t>> Descriptors(const Bytes& vbmeta) {
    auto h = Header(vbmeta);
    uint64_t base = 256 + h.authentication_data_block_size;
    Bounds(h.descriptors_offset, h.descriptors_size, h.auxiliary_data_block_size);
    uint64_t pos = base + h.descriptors_offset, end = pos + h.descriptors_size;
    std::vector<std::pair<size_t, size_t>> result;
    while (pos < end) {
        Bounds(pos, sizeof(AvbDescriptor), end);
        AvbDescriptor d;
        Check(avb_descriptor_validate_and_byteswap(reinterpret_cast<const AvbDescriptor*>(vbmeta.data() + pos), &d),
              "invalid AVB descriptor");
        Check(d.num_bytes_following <= end - pos - sizeof(d), "AVB descriptor exceeds blob");
        size_t length = sizeof(d) + d.num_bytes_following;
        result.emplace_back(pos, length); pos += length;
    }
    Check(pos == end, "AVB descriptor boundary mismatch");
    return result;
}
std::unique_ptr<RSA, decltype(&RSA_free)> LoadRsa(const std::string& pem) {
    Check(!pem.empty(), "a signing key is required for this partition");
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), pem.size()), BIO_free);
    Check(bio != nullptr, "cannot create signing key buffer");
    auto key = std::unique_ptr<RSA, decltype(&RSA_free)>(
        PEM_read_bio_RSAPrivateKey(bio.get(), nullptr, nullptr, nullptr), RSA_free);
    Check(key != nullptr, "cannot parse RSA signing key");
    return key;
}
void MatchKey(const Bytes& vbmeta, RSA* rsa) {
    auto pub = PublicKey(vbmeta);
    Check(pub.size() >= 8, "missing AVB public key");
    uint32_t bits; memcpy(&bits, pub.data(), 4); bits = avb_be32toh(bits);
    Check(bits == static_cast<uint32_t>(RSA_bits(rsa)) && pub.size() == 8 + bits / 4,
          "signing key does not match AVB public key size");
    const BIGNUM *n, *e; RSA_get0_key(rsa, &n, &e, nullptr);
    Bytes modulus(bits / 8);
    Check(BN_bn2binpad(n, modulus.data(), modulus.size()) == static_cast<int>(modulus.size()) &&
          BN_get_word(e) == 65537 &&
          std::equal(modulus.begin(), modulus.end(), pub.begin() + 8),
          "signing key does not match the original trusted key");
}
}  // namespace

AvbVBMetaImageHeader Header(const Bytes& vbmeta) {
    Check(vbmeta.size() >= 256 && memcmp(vbmeta.data(), "AVB0", 4) == 0, "missing VBMeta header");
    AvbVBMetaImageHeader h;
    avb_vbmeta_image_header_to_host_byte_order(reinterpret_cast<const AvbVBMetaImageHeader*>(vbmeta.data()), &h);
    Bounds(256, h.authentication_data_block_size, vbmeta.size());
    Bounds(256 + h.authentication_data_block_size, h.auxiliary_data_block_size, vbmeta.size());
    Check(h.flags == 0, "disabled AVB verification is not supported");
    return h;
}
Bytes ReadVbmeta(int fd, uint64_t offset, uint64_t available) {
    Check(available >= 256, "truncated VBMeta");
    auto head = Read(fd, offset, 256);
    Check(memcmp(head.data(), "AVB0", 4) == 0, "VBMeta magic mismatch");
    AvbVBMetaImageHeader h;
    avb_vbmeta_image_header_to_host_byte_order(reinterpret_cast<const AvbVBMetaImageHeader*>(head.data()), &h);
    Check(h.authentication_data_block_size <= kMiB && h.auxiliary_data_block_size <= 16 * kMiB,
          "VBMeta is too large");
    uint64_t total = 256 + h.authentication_data_block_size + h.auxiliary_data_block_size;
    Check(total <= available, "VBMeta exceeds partition");
    auto blob = Read(fd, offset, total); Header(blob); return blob;
}
PartitionAvb ReadPartitionAvb(const std::string& image) {
    auto fd = OpenRead(image); uint64_t size = Size(fd);
    Check(size >= 64, "truncated partition image");
    auto bytes = Read(fd, size - 64, 64); AvbFooter footer;
    Check(avb_footer_validate_and_byteswap(reinterpret_cast<const AvbFooter*>(bytes.data()), &footer),
          "missing or invalid AVB footer: " + image);
    Bounds(footer.vbmeta_offset, footer.vbmeta_size, size - 64);
    Check(footer.original_image_size <= footer.vbmeta_offset, "invalid original data size");
    auto blob = ReadVbmeta(fd, footer.vbmeta_offset, footer.vbmeta_size);
    Check(blob.size() == footer.vbmeta_size, "AVB footer size mismatch");
    return {std::move(blob), footer.original_image_size};
}
Bytes PublicKey(const Bytes& vbmeta) {
    auto h = Header(vbmeta);
    Bounds(h.public_key_offset, h.public_key_size, h.auxiliary_data_block_size);
    size_t pos = 256 + h.authentication_data_block_size + h.public_key_offset;
    return Bytes(vbmeta.begin() + pos, vbmeta.begin() + pos + h.public_key_size);
}
std::vector<TreeDescriptor> Trees(const Bytes& vbmeta) {
    std::vector<TreeDescriptor> result;
    for (auto [offset, size] : Descriptors(vbmeta)) {
        auto* d = reinterpret_cast<const AvbDescriptor*>(vbmeta.data() + offset);
        if (avb_be64toh(d->tag) != AVB_DESCRIPTOR_TAG_HASHTREE) continue;
        Check(size >= sizeof(AvbHashtreeDescriptor), "short hashtree descriptor");
        TreeDescriptor tree{}; tree.offset = offset;
        Check(avb_hashtree_descriptor_validate_and_byteswap(
            reinterpret_cast<const AvbHashtreeDescriptor*>(d), &tree.header), "invalid hashtree descriptor");
        auto& h = tree.header;
        uint64_t names = uint64_t(h.partition_name_len) + h.salt_len + h.root_digest_len;
        Bounds(sizeof(h), names, size);
        const char* name = reinterpret_cast<const char*>(d) + sizeof(h);
        tree.name.assign(name, h.partition_name_len);
        Check(SafeRelative(tree.name).find('/') == std::string::npos, "invalid partition name");
        tree.algorithm.assign(reinterpret_cast<const char*>(h.hash_algorithm),
                              strnlen(reinterpret_cast<const char*>(h.hash_algorithm), 32));
        const uint8_t* salt = reinterpret_cast<const uint8_t*>(name) + h.partition_name_len;
        tree.salt.assign(salt, salt + h.salt_len);
        tree.root.assign(salt + h.salt_len, salt + h.salt_len + h.root_digest_len);
        Check(h.dm_verity_version == 1 && !tree.root.empty(), "unsupported persistent verity digest");
        result.push_back(std::move(tree));
    }
    return result;
}
std::vector<ChainDescriptor> Chains(const Bytes& vbmeta) {
    std::vector<ChainDescriptor> result;
    for (auto [offset, size] : Descriptors(vbmeta)) {
        auto* d = reinterpret_cast<const AvbDescriptor*>(vbmeta.data() + offset);
        if (avb_be64toh(d->tag) != AVB_DESCRIPTOR_TAG_CHAIN_PARTITION) continue;
        Check(size >= sizeof(AvbChainPartitionDescriptor), "short chain descriptor");
        AvbChainPartitionDescriptor chain;
        Check(avb_chain_partition_descriptor_validate_and_byteswap(
            reinterpret_cast<const AvbChainPartitionDescriptor*>(d), &chain), "invalid AVB chain");
        Bounds(sizeof(chain), uint64_t(chain.partition_name_len) + chain.public_key_len, size);
        const uint8_t* name = reinterpret_cast<const uint8_t*>(d) + sizeof(chain);
        ChainDescriptor out{std::string(reinterpret_cast<const char*>(name), chain.partition_name_len), {}};
        Check(SafeRelative(out.name).find('/') == std::string::npos, "invalid chain name");
        out.key.assign(name + chain.partition_name_len, name + chain.partition_name_len + chain.public_key_len);
        result.push_back(std::move(out));
    }
    return result;
}
void VerifySignature(const Bytes& vbmeta, bool allow_unsigned) {
    auto result = avb_vbmeta_image_verify(vbmeta.data(), vbmeta.size(), nullptr, nullptr);
    Check(result == AVB_VBMETA_VERIFY_RESULT_OK ||
          (allow_unsigned && result == AVB_VBMETA_VERIFY_RESULT_OK_NOT_SIGNED),
          std::string("AVB signature verification failed: ") + avb_vbmeta_verify_result_to_string(result));
    Header(vbmeta);
}
void CheckSigningKey(const Bytes& vbmeta, const std::string& pem) {
    auto key = LoadRsa(pem); MatchKey(vbmeta, key.get());
}
void Sign(Bytes& vbmeta, const std::string& pem) {
    auto h = Header(vbmeta);
    if (h.algorithm_type == AVB_ALGORITHM_TYPE_NONE) return;
    Check(h.algorithm_type == AVB_ALGORITHM_TYPE_SHA256_RSA2048 ||
          h.algorithm_type == AVB_ALGORITHM_TYPE_SHA256_RSA4096, "unsupported AVB signing algorithm");
    auto key = LoadRsa(pem); MatchKey(vbmeta, key.get());
    Bytes message(vbmeta.begin(), vbmeta.begin() + 256);
    message.insert(message.end(), vbmeta.begin() + 256 + h.authentication_data_block_size, vbmeta.end());
    auto hash = Digest("sha256", message.data(), message.size());
    Bounds(h.hash_offset, h.hash_size, h.authentication_data_block_size);
    Bounds(h.signature_offset, h.signature_size, h.authentication_data_block_size);
    Check(h.hash_size == hash.size() && h.signature_size == static_cast<uint64_t>(RSA_size(key.get())),
          "invalid authentication block layout");
    std::copy(hash.begin(), hash.end(), vbmeta.begin() + 256 + h.hash_offset);
    unsigned length = 0;
    Check(RSA_sign(NID_sha256, hash.data(), hash.size(), vbmeta.data() + 256 + h.signature_offset,
                   &length, key.get()) == 1 && length == h.signature_size, "RSA signing failed");
    VerifySignature(vbmeta);
}
void VerifyTree(const std::string& image, const TreeDescriptor& tree) {
    auto fd = OpenRead(image);
    Bounds(0, tree.header.image_size, Size(fd));
    Bounds(tree.header.tree_offset, tree.header.tree_size, Size(fd));
    auto generated = BuildTree(image, tree.header.image_size, tree);
    Check(generated.root == tree.root, "verity root mismatch: " + tree.name);
    Check(generated.bytes.size() == tree.header.tree_size &&
          generated.bytes == Read(fd, tree.header.tree_offset, tree.header.tree_size),
          "verity tree mismatch: " + tree.name);
}
Bytes SealPartition(const std::string& image, Bytes blob, const std::string& key) {
    auto trees = Trees(blob); Check(trees.size() == 1, "expected one partition hashtree descriptor");
    auto tree = trees.front();
    uint64_t data_size = FileSize(image);
    Progress("Generating verity tree for " + tree.name);
    auto generated = BuildTree(image, data_size, tree);
    Check(generated.root.size() == tree.root.size(), "verity digest size changed");
    auto fd = OpenWrite(image);
    uint64_t tree_offset = data_size, fec_offset = tree_offset + generated.bytes.size();
    Write(fd, tree_offset, generated.bytes);
    Resize(image, fec_offset);
    Progress("Generating FEC for " + tree.name);
    auto fec = EncodeFec(image, fec_offset, tree.header.fec_num_roots);
    Write(fd, fec_offset, fec);
    uint64_t vbmeta_offset = Align(fec_offset + fec.size(), 4096);
    auto* d = reinterpret_cast<AvbHashtreeDescriptor*>(blob.data() + tree.offset);
    d->image_size = avb_htobe64(data_size);
    d->tree_offset = avb_htobe64(tree_offset);
    d->tree_size = avb_htobe64(generated.bytes.size());
    d->fec_offset = avb_htobe64(fec.empty() ? 0 : fec_offset);
    d->fec_size = avb_htobe64(fec.size());
    size_t root_offset = tree.offset + sizeof(*d) + tree.header.partition_name_len + tree.header.salt_len;
    std::copy(generated.root.begin(), generated.root.end(), blob.begin() + root_offset);
    Sign(blob, key); VerifySignature(blob, true);
    Write(fd, vbmeta_offset, blob);
    uint64_t partition_size = Align(vbmeta_offset + Align(blob.size(), 4096) + 4096, 4096);
    Resize(image, partition_size);
    AvbFooter footer{};
    memcpy(footer.magic, "AVBf", 4); footer.version_major = avb_htobe32(1);
    footer.original_image_size = avb_htobe64(data_size);
    footer.vbmeta_offset = avb_htobe64(vbmeta_offset); footer.vbmeta_size = avb_htobe64(blob.size());
    const auto* bytes = reinterpret_cast<const uint8_t*>(&footer);
    Write(fd, partition_size - 64, Bytes(bytes, bytes + 64));
    Check(fsync(fd) == 0, "partition fsync failed");
    VerifyTree(image, Trees(blob).front());
    // Read back the parity after writing; independent reference-tool tests also compare FEC bytes.
    Check(Read(fd, fec_offset, fec.size()) == fec, "FEC readback mismatch");
    return blob;
}
void ReplaceTree(Bytes& top, const TreeDescriptor& target, const Bytes& partition_vbmeta) {
    auto trees = Trees(partition_vbmeta);
    Check(trees.size() == 1 && trees[0].name == target.name, "wrong replacement descriptor");
    auto source = trees.front();
    size_t a = sizeof(AvbDescriptor) + target.header.parent_descriptor.num_bytes_following;
    size_t b = sizeof(AvbDescriptor) + source.header.parent_descriptor.num_bytes_following;
    Check(a == b, "AVB descriptor size changed");
    std::copy(partition_vbmeta.begin() + source.offset, partition_vbmeta.begin() + source.offset + b,
              top.begin() + target.offset);
}
std::pair<std::string, uint64_t> VbmetaDigest(const Bytes& top,
                                            const std::map<std::string, std::string>& partitions) {
    Bytes data = top;
    for (const auto& chain : Chains(top)) {
        Check(partitions.count(chain.name), "chained partition is absent");
        auto child = ReadPartitionAvb(partitions.at(chain.name)).vbmeta;
        VerifySignature(child);
        Check(PublicKey(child) == chain.key, "chained public key mismatch");
        Check(Chains(child).empty(), "nested AVB chains are not supported");
        data.insert(data.end(), child.begin(), child.end());
    }
    return {Hex(Digest("sha256", data.data(), data.size())), data.size()};
}
}  // namespace kemu
