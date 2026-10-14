// Copyright 2026 The Kemu Authors
// SPDX-License-Identifier: Apache-2.0
#include "avb.h"
#include "filesystem.h"
#include "layout.h"
#include "test_keys.h"
#include <sys/statvfs.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <set>

namespace kemu {
namespace fs = std::filesystem;
struct Options {
    std::string input, manifest, payload, output;
    std::string tools = "/system/bin";
};
void Usage() {
    std::cout << "kemu_image_burner --input system.img --manifest update.json --payload-dir DIR\n"
                 "  --output-dir NEW_DIR [--tools-dir DIR]\n"
                 "\nEdits an offline Kemu GPT/super image. Never writes the input or a block device.\n"
                 "Tools default to /system/bin (e2fsck and resize2fs); override for a standalone kit.\n"
                 "Output: system.img, VerifiedBootParams.textproto and result.json.\n";
}
Options Parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        Check(i + 1 < argc, "missing option value: " + option);
        std::string value = argv[++i];
        if (option == "--input") o.input = value;
        else if (option == "--manifest") o.manifest = value;
        else if (option == "--payload-dir") o.payload = value;
        else if (option == "--output-dir") o.output = value;
        else if (option == "--tools-dir") o.tools = value;
        else Check(false, "unknown option: " + option);
    }
    Check(!o.input.empty() && !o.manifest.empty() && !o.payload.empty() && !o.output.empty() &&
          !o.tools.empty(), "missing required options (use --help)");
    o.input = fs::absolute(o.input).string(); o.output = fs::absolute(o.output).string();
    o.tools = fs::absolute(o.tools).string();
    return o;
}
bool HashString(const std::string& hash) {
    return hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos;
}
void Burn(const Options& o) {
    SetFilesystemTool(o.tools + "/kemu_ext4_tool");
    Check(access((o.tools + "/kemu_ext4_tool").c_str(), X_OK) == 0, "ext4 helper is not executable");
    auto manifest = LoadJson(o.manifest);
    Check(manifest["schema_version"].isInt() && manifest["schema_version"].asInt() == 1,
          "unsupported update schema");
    Check(manifest["base_image_sha256"].isString() && HashString(manifest["base_image_sha256"].asString()),
          "base_image_sha256 must be a 64-character lowercase SHA-256 string");
    Check(manifest["files"].isArray() && !manifest["files"].empty() && manifest["files"].size() <= 4096,
          "files must be a nonempty array");
    std::map<std::string, std::vector<Replacement>> changes;
    std::set<std::string> paths;
    for (const auto& entry : manifest["files"]) {
        Check(entry["path"].isString() && entry["source"].isString() && entry["sha256"].isString(),
              "file entry requires path, source and sha256 strings");
        Replacement file{entry["path"].asString(), PayloadPath(o.payload, entry["source"].asString()),
                         entry["sha256"].asString()};
        Check(file.path.size() > 1 && file.path[0] == '/',
              "only absolute guest file paths can be replaced");
        auto relative = SafeRelative(file.path.substr(1)); size_t slash = relative.find('/');
        Check(slash != std::string::npos && paths.insert(file.path).second, "invalid or duplicate guest path");
        std::string partition = relative.substr(0, slash);
        Check(partition == "system" || partition == "vendor" || partition == "system_ext" || partition == "product",
              "only ext4 system/vendor/system_ext/product files are supported");
        Check(HashString(file.sha256), "invalid SHA-256");
        Check(FileSize(file.source) <= 512 * kMiB && FileHash(file.source) == file.sha256,
              "payload hash mismatch: " + file.path);
        changes[partition].push_back(std::move(file));
    }
    Check(access((o.tools + "/e2fsck").c_str(), X_OK) == 0 &&
          access((o.tools + "/resize2fs").c_str(), X_OK) == 0, "e2fsck/resize2fs are not executable");
    Check(!fs::exists(o.output) && fs::is_directory(fs::path(o.output).parent_path()),
          "output must be a new directory under an existing parent");
    auto gpt = ReadGpt(o.input); auto input = OpenRead(o.input);
    auto top = ReadVbmeta(input, gpt.vbmeta_offset, gpt.vbmeta_size);
    VerifySignature(top); CheckSigningKey(top, kVbmetaTestKey);
    Progress("Checking input image and update payloads");
    std::string original_hash = FileHash(o.input);
    Check(manifest["base_image_sha256"].asString() == original_hash, "base image hash mismatch");
    Check(fs::create_directory(o.output), "cannot create output directory");
    fs::permissions(o.output, fs::perms::owner_all);
    std::string work = o.output + "/.work"; Check(fs::create_directory(work), "cannot create work directory");
    struct statvfs disk{};
    Check(statvfs(work.c_str(), &disk) == 0, "cannot check working storage");
    Check(uint64_t(disk.f_bavail) * disk.f_frsize > 4 * FileSize(o.input) + 512 * kMiB,
          "insufficient scratch space (need at least 4x input size + 512 MiB)");
    std::map<std::string, std::string> partitions;
    Progress("Extracting logical partitions");
    auto meta = ExtractPartitions(o.input, gpt, work + "/partitions", partitions);
    std::map<std::string, Bytes> blobs;
    for (const auto& tree : Trees(top)) {
        Check(partitions.count(tree.name), "top-level hashtree partition missing: " + tree.name);
        VerifyTree(partitions.at(tree.name), tree);
    }
    for (const auto& chain : Chains(top)) {
        Check(partitions.count(chain.name), "chained partition missing");
        auto blob = ReadPartitionAvb(partitions.at(chain.name)).vbmeta;
        VerifySignature(blob); Check(PublicKey(blob) == chain.key, "input chained public key mismatch");
        Check(Chains(blob).empty(), "nested chains are not supported");
        for (const auto& tree : Trees(blob)) {
            Check(partitions.count(tree.name), "chain hashtree partition missing");
            VerifyTree(partitions.at(tree.name), tree);
        }
    }
    for (const auto& [partition, replacements] : changes) {
        Check(partitions.count(partition), "requested partition missing: " + partition);
        auto& path = partitions.at(partition); auto avb = ReadPartitionAvb(path);
        VerifySignature(avb.vbmeta, true);
        auto trees = Trees(avb.vbmeta);
        Check(trees.size() == 1 && trees.front().name == partition && trees.front().header.image_size == avb.data_size,
              "unexpected partition AVB structure");
        bool trusted = false;
        for (const auto& chain : Chains(top))
            if (chain.name == partition && chain.key == PublicKey(avb.vbmeta)) trusted = true;
        for (const auto& tree : Trees(top))
            if (tree.name == partition && tree.root == trees.front().root &&
                tree.header.image_size == avb.data_size) trusted = true;
        Check(trusted, "replacement partition is not covered by the trusted AVB metadata");
        std::string key = partition == "system" ? kSystemTestKey : "";
        if (Header(avb.vbmeta).algorithm_type != AVB_ALGORITHM_TYPE_NONE) CheckSigningKey(avb.vbmeta, key);
        Progress("Preparing writable ext4 copy: " + partition);
        Resize(path, avb.data_size);
        uint64_t working_size = WorkingFilesystemSize(path, replacements);
        Check(Run({o.tools + "/resize2fs", "-f", path, std::to_string(working_size / 4096)}) == 0,
              "filesystem expansion failed");
        Fsck(o.tools + "/e2fsck", path, true);
        ReplaceFiles(path, partition, replacements);
        Fsck(o.tools + "/e2fsck", path);
        // Remove scratch slack before adding new AVB data. No previous partition size is assumed.
        Check(Run({o.tools + "/resize2fs", "-M", path}) == 0, "filesystem minimization failed");
        Fsck(o.tools + "/e2fsck", path);
        blobs[partition] = SealPartition(path, avb.vbmeta, key);
    }
    for (const auto& tree : Trees(top))
        if (blobs.count(tree.name)) ReplaceTree(top, tree, blobs.at(tree.name));
    Sign(top, kVbmetaTestKey);
    auto [digest, vbmeta_size] = VbmetaDigest(top, partitions);
    std::string super = work + "/super.output.img", output = o.output + "/system.img";
    Progress("Rebuilding super metadata and physical GPT image");
    PackSuper(*meta, partitions, super); PackDisk(gpt, top, super, output);
    Check(fs::remove(super), "cannot remove super work image");
    Progress("Verifying the assembled image and all partition bytes");
    VerifyPacked(output, top, partitions, work + "/readback");
    Check(FileHash(o.input) == original_hash, "input image changed during operation");
    std::string params = "major_version: 2\nparam: \"androidboot.vbmeta.size=" + std::to_string(vbmeta_size) +
        "\"\nparam: \"androidboot.vbmeta.hash_alg=sha256\"\nparam: \"androidboot.vbmeta.digest=" + digest + "\"\n";
    SaveText(o.output + "/VerifiedBootParams.textproto", params);
    Json::Value result;
    result["schema_version"] = 1; result["status"] = "ready";
    result["base_image_sha256"] = original_hash; result["image_sha256"] = FileHash(output);
    result["verified_boot_params_sha256"] = FileHash(o.output + "/VerifiedBootParams.textproto");
    result["image_size"] = Json::UInt64(FileSize(output)); result["vbmeta_digest"] = digest;
    result["vbmeta_size"] = Json::UInt64(vbmeta_size); result["files"] = manifest["files"];
    result["verification"] = "GPT CRCs; liblp metadata; AVB signatures and original trusted keys; verity trees; payload SHA-256; assembled partition byte hashes";
    for (const auto& [name, path] : partitions) {
        result["partitions"][name]["size"] = Json::UInt64(FileSize(path));
        result["partitions"][name]["sha256"] = FileHash(path);
        Check(fs::remove(path), "cannot remove partition work file");
    }
    Check(fs::remove(work + "/partitions") && fs::remove(work), "cannot remove work directories");
    // Written last: consumers must require this marker and verify the output hashes before switching.
    SaveJson(o.output + "/result.json", result);
    Progress("Ready: " + o.output);
}
}  // namespace kemu
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") { kemu::Usage(); return 0; }
        kemu::Burn(kemu::Parse(argc, argv)); return 0;
    } catch (const std::exception& error) {
        std::cerr << "kemu_image_burner: " << error.what() << std::endl;
        return 1;
    }
}
