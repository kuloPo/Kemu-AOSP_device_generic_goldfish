# Kemu Android Image Burner

`kemu_image_burner` replaces existing regular files in an offline Kemu image from an ordinary working directory on Android x86_64. It produces a new disk image and verified boot parameters for export. It does not mount or modify the running system, require `adb root`, disable SELinux or AVB, or require partition offsets and sizes to remain unchanged.

## Build and installation

The source lives in `tools/image_burner/` in the Goldfish device repository. The Digitalis product includes the tool through `PRODUCT_PACKAGES`. Use the existing Kemu build workflow:

```sh
source build/envsetup.sh
lunch sdk_phone64_x86_64_digitalis-trunk_staging-userdebug
make -j"$(nproc)"
```

No additional build target, manual binary installation, or change to `copy_imgs.sh` is needed. The build installs `out/target/product/emu64xa/system/bin/kemu_image_burner` into the system logical partition and the distribution image, `system-qemu.img`. After copying the images as usual, run `/system/bin/kemu_image_burner` in the emulator.

The module's `required` dependencies install `/system/bin/kemu_ext4_tool`, `/system/bin/e2fsck`, `/system/bin/resize2fs`, and toybox (providing `/system/bin/sha256sum`) automatically. `--tools-dir` defaults to `/system/bin`. The executable uses system libraries without requiring vendor variants. The device SELinux policy allows shell to read and execute the filesystem helpers labeled `fsck_exec`, without granting mount or block-device access. Execution remains in the shell domain with SELinux enforcing.

Signing uses two publicly known AOSP test private keys embedded in `test_keys.h`: `testkey_rsa4096.pem` for vbmeta and `testkey_rsa2048.pem` for system. No PEM files are needed, and `--key` is not supported. The original image's public keys must match; the tool does not replace trust roots or disable verification. These embedded keys are not production secrets. Changing them requires editing the source and rebuilding.

### Optional host tests and standalone kit

For host tests or a standalone kit to push to an older image, also run:

```sh
m -j8 kemu_image_burner_host kemu_ext4_tool e2fsck_static resize2fs_ramdisk
python3 device/generic/goldfish/tools/image_burner/package_tool.py \
  --product-out out/target/product/emu64xa \
  --output /tmp/kemu-image-burner-kit
```

The kit includes the standalone ext4 editor and two static ext4 utilities. Point `--tools-dir` to the kit directory when running it. It uses the same embedded test keys and does not include separate PEM files. Python and the OpenSSL command-line tool are not needed inside the emulator.

## Update jobs

The Windows updater is responsible for resolving release manifests across versions, downloading the final version of each target file into a payload directory, and generating a job like this:

```json
{
  "schema_version": 1,
  "base_image_sha256": "REPLACE_WITH_INPUT_IMAGE_SHA256",
  "files": [
    {
      "path": "/vendor/lib64/hw/vulkan.ranchu.so",
      "source": "vulkan.ranchu.so",
      "sha256": "REPLACE_WITH_PAYLOAD_SHA256"
    }
  ]
}
```

Replace both hash placeholders with the actual 64-character lowercase hexadecimal SHA-256 values. Payloads are complete files, not binary patches.

- `schema_version` identifies the job format and must be the integer `1`.
- `path` is the absolute target path in Android.
- `source` is relative to `--payload-dir`; paths escaping that directory are rejected.
- `sha256` is required and should come from a trusted release manifest. The updater is responsible for authenticating the download.
- `base_image_sha256` is required and covers the complete input `system.img`. Missing, malformed, or mismatched hashes are rejected. The input is checked again before completion to ensure it has not changed.

Push the current distribution's `system-img/system.img`, the job, and the payloads to a separate working directory in the emulator. The built-in tool does not need to be pushed again. The input must be the distribution image containing GPT, vbmeta, and super, not the similarly named ext4 logical partition image in the AOSP product directory.

Run inside the emulator:

```sh
/system/bin/kemu_image_burner \
  --input /data/local/tmp/kemu-burn/input/system.img \
  --manifest /data/local/tmp/kemu-burn/update.json \
  --payload-dir /data/local/tmp/kemu-burn/payload \
  --output-dir /data/local/tmp/kemu-burn/output
```

The output directory must not exist, and its parent directory must exist. Use a filesystem with sufficient free space and, for a standalone kit, permission to execute the pushed tools. Processing regular files does not require root. Reading protected block devices is outside the tool's scope.

Successful output contains:

- `system.img`: the rebuilt complete GPT disk image.
- `VerifiedBootParams.textproto`: boot parameters recalculated from the new signed metadata chain.
- `result.json`: the completion marker, written last, containing input, output, partition, and payload verification information. `base_image_sha256` is the complete input image hash; `image_sha256` is the complete output `system.img` hash. Recalculate and compare the latter after export.

The updater must require `status: "ready"` in `result.json`, verify the exported hashes, shut down the emulator, and switch the image and boot parameters together as one transaction. Cold boot the new image instead of restoring a snapshot from the previous system. Keep the previous image set until health checks pass. The tool does not automatically modify host images, the running emulator, or user data.

Failures return a nonzero exit code without a completion marker. Intermediate `.work` files are retained for diagnosis when present. Remove the failed output directory before reusing the same output path. Successful runs remove intermediate partition images.

## Supported layout and operations

The implementation targets the current Kemu image layout:

- GPT with 512-byte sectors and exactly two physical partitions, `vbmeta` and `super`. Both primary and backup GPT CRCs are checked.
- Single-device super without Virtual A/B. Logical partition names, attributes, and groups are preserved; extents are reassigned, and super/GPT can grow when necessary.
- Replacement of existing, singly linked regular files in `system`, `vendor`, `system_ext`, and `product`. Symbolic links are not followed, and files with multiple hard links are rejected.
- Ext4 with 4096-byte blocks. Working copies are expanded, shared blocks are unshared with e2fsck, replacements preserve the existing inode, and unused space is then reduced.
- Preservation of AVB properties, salts, rollback indexes, and public keys. SHA-1/SHA-256 hashtrees, FEC, SHA256_RSA2048/SHA256_RSA4096 signatures, and one level of chained partitions are supported.
- Unmodified logical partitions are preserved byte for byte. Modified filesystems need not match the layout of a server-built image; use the actual output AVB digest.
- No file creation or deletion, editing files inside APK/APEX containers, EROFS updates, encrypted userdata processing, A/B OTA slot handling, or multi-device super support.

Unsupported layouts are rejected rather than overwritten at raw offsets. Hashtree partition data is verified only for partitions present in the input image. Hash descriptors referencing external boot partitions are preserved, but their data cannot be verified without those partition files.

Several temporary images are needed. Before processing begins, free space on the output filesystem must exceed four times the input size plus 512 MiB. Fully unsharing filesystem blocks can require additional space. Insufficient space is a failure. The input file is never modified.

## Integration tests

Host integration tests independently verify output using AOSP `mke2fs`, `avbtool`, `fec`, `lpmake`, `lpdump`, and `debugfs`:

```sh
python3 device/generic/goldfish/tools/image_burner/test_image_burner.py \
  --aosp "$PWD" --binary out/host/linux-x86/bin/kemu_image_burner_host \
  --work /tmp/kemu-burner-test
```

Tests cover both hash and signature algorithms, chained digests, byte-for-byte FEC comparison, growing shared libraries, text and executable files, empty files, rejection of missing targets/directories/symbolic and hard links, partition relayout, input preservation, output hashes, and rejection of missing or invalid input hashes, mismatched payload hashes, and path traversal. The work directory must not already exist.

## Process boundary

The burner does not link libext2fs. It invokes `kemu_ext4_tool` through argument arrays without a shell. The editor accepts ordinary raw ext4 images and supports standalone commands:

```sh
kemu_ext4_tool size IMAGE TOTAL_PAYLOAD_BYTES
kemu_ext4_tool replace IMAGE /PATH/IN/FILESYSTEM SOURCE_FILE
kemu_ext4_tool extract IMAGE /PATH/IN/FILESYSTEM NEW_OUTPUT_FILE
```

`replace` only changes existing, singly linked regular files. It accepts arbitrary file bytes, including empty files, preserves the inode, and rejects symlink traversal. It does not validate executable architecture or application-level file formats. Paths refer to the filesystem itself (for example, `/lib64/hw/vulkan.ranchu.so` inside a vendor partition). The tool does not understand update manifests, release versions, GPT, super, or AVB. It only edits a working copy prepared by the caller; it provides no transaction rollback itself.

After each replacement, the burner starts a separate extraction command and hashes the extracted bytes using `sha256sum`. The temporary file is deleted after a matching hash. Input images, payloads, and output images are also hashed by `sha256sum`; AVB cryptography remains in the main process. Failed commands, malformed responses, and readback mismatches prevent creation of the completion marker. Host builds use `/usr/bin/sha256sum`; Android builds use `/system/bin/sha256sum`.

The editor's own source (`ext4_tool.cpp` and `ext4_support.h`) is MIT-licensed. Its linked e2fsprogs code retains its upstream GPL/LGPL and other notices. Separating executables does not remove the obligation to distribute applicable notices and corresponding source for third-party components. See `LICENSE.ext4-tool` for the editor source license.
