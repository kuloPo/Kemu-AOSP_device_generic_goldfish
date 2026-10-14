#!/usr/bin/env python3
"""Package already-built Android tools; no private production keys are copied."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--product-out', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    product, output = args.product_out.resolve(), args.output.resolve()
    # Partial module builds do not necessarily refresh product module-info.json.
    binaries = {'kemu_ext4_tool': product / 'system/bin/kemu_ext4_tool', 'kemu_image_burner': product / 'system/bin/kemu_image_burner',
                'e2fsck': product / 'system/bin/e2fsck_static',
                'resize2fs': product / 'ramdisk/system/bin/resize2fs'}
    for name, path in binaries.items():
        with path.open('rb') as stream:
            header = stream.read(20)
        if header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\x3e\x00':
            raise RuntimeError(f'{name} is not an Android x86_64 executable: {path}')
    output.mkdir(exist_ok=False)
    for name, path in binaries.items():
        shutil.copyfile(path, output / name)
        (output / name).chmod(0o755)
    for name in ['README.md', 'LICENSE.ext4-tool']:
        shutil.copyfile(Path(__file__).with_name(name), output / name)
    files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir() if p.is_file()}
    (output / 'tool-manifest.json').write_text(json.dumps({'schema_version': 1, 'architecture': 'android-x86_64', 'files': files}, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
