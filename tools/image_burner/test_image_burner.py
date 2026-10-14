#!/usr/bin/env python3
"""Independent AVB/FEC/GPT integration checks using the AOSP reference tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib


def run(args, *, expected=(0,), env=None):
    result = subprocess.run([str(x) for x in args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, env=env)
    if result.returncode not in expected:
        raise AssertionError(f"Command failed ({result.returncode}): {args}\n{result.stdout[-12000:]}")
    return result.stdout


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def pack_gpt(path, super_path, vbmeta_path):
    sector = 512
    super_start = 2 << 20
    disk_size = (super_start + super_path.stat().st_size + 16896 + (1 << 20) - 1) // (1 << 20) * (1 << 20)
    last = disk_size // sector - 1
    entries = bytearray(128 * 128)
    for index, (name, start, size) in enumerate([('vbmeta', 1 << 20, 1 << 20),
                                               ('super', super_start, super_path.stat().st_size)]):
        offset = index * 128
        entries[offset:offset + 16] = bytes.fromhex('28732ac11ff8d211ba4b00a0c93ec93b')
        entries[offset + 16:offset + 32] = bytes([index + 1]) * 16
        struct.pack_into('<QQQ', entries, offset + 32, start // sector, (start + size) // sector - 1, 0)
        text = name.encode('utf-16le')
        entries[offset + 56:offset + 56 + len(text)] = text

    def header(current, alternate, table):
        h = bytearray(512)
        struct.pack_into('<8sIIIIQQQQ16sQIII', h, 0, b'EFI PART', 0x10000, 92, 0, 0,
                         current, alternate, 34, last - 33, b'kemu-test-image!', table,
                         128, 128, zlib.crc32(entries))
        struct.pack_into('<I', h, 16, zlib.crc32(h[:92]))
        return h

    mbr = bytearray(512)
    mbr[450] = 0xee
    struct.pack_into('<II', mbr, 454, 1, min(last, 0xffffffff))
    mbr[510:512] = b'\x55\xaa'
    with path.open('xb') as f:
        f.write(mbr); f.write(header(1, last, 2)); f.write(entries)
        f.seek(1 << 20); f.write(vbmeta_path.read_bytes())
        f.seek(super_start)
        with super_path.open('rb') as src:
            shutil.copyfileobj(src, f, 4 << 20)
        f.seek((last - 32) * sector); f.write(entries); f.write(header(last, 1, last - 32))


def unpack(image, destination, lpdump):
    destination.mkdir()
    with image.open('rb') as source:
        source.seek(512); h = source.read(512)
        source.seek(struct.unpack_from('<Q', h, 72)[0] * 512)
        entries = source.read(struct.unpack_from('<I', h, 80)[0] * 128)
        named = {}
        for offset in range(0, len(entries), 128):
            if not any(entries[offset:offset + 16]):
                continue
            name = entries[offset + 56:offset + 128].decode('utf-16le').rstrip('\0')
            first, last = struct.unpack_from('<QQ', entries, offset + 32)
            named[name] = (first * 512, (last - first + 1) * 512)
        for name in ['vbmeta', 'super']:
            start, size = named[name]
            source.seek(start)
            with (destination / (name + '.img')).open('xb') as out:
                remaining = size
                while remaining:
                    chunk = source.read(min(4 << 20, remaining)); assert chunk
                    out.write(chunk); remaining -= len(chunk)
    import re
    layout = run([lpdump, destination / 'super.img'])
    for section in layout.split('Super partition layout:')[0].split('  Name: ')[1:]:
        name = section.splitlines()[0].strip()
        with (destination / (name + '.img')).open('xb') as out, (destination / 'super.img').open('rb') as source:
            for start, end, physical in re.findall(r'(\d+) \.\. (\d+) linear super (\d+)', section):
                start, end, physical = int(start), int(end), int(physical)
                source.seek(physical * 512); out.seek(start * 512)
                out.write(source.read((end - start + 1) * 512))
    return named, layout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aosp', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    root, work = args.aosp.resolve(), args.work.resolve()
    work.mkdir(exist_ok=False)
    host = root / 'out/host/linux-x86/bin'
    avb = host / 'avbtool'
    env = dict(os.environ, PATH=str(host) + ':' + os.environ.get('PATH', ''))
    keys = root / 'external/avb/test/data'
    old = root / 'out/target/product/emu64xa/system/lib64/libprintspooler_jni.so'
    assert old.is_file()
    payload = work / 'payload'; payload.mkdir()
    shutil.copyfile(old, payload / 'small.so')
    (payload / 'large.so').write_bytes(old.read_bytes() + bytes(2 << 20))
    regular = {'config.txt': b'new=value\n' * 500, 'empty': b'', 'program': (root / 'out/target/product/emu64xa/system/bin/toybox').read_bytes()}
    for name, data in regular.items():
        (payload / name).write_bytes(data)
    for name in ['system', 'vendor']:
        tree = work / (name + '-tree')
        lib = tree / ('system/lib64' if name == 'system' else 'lib64')
        lib.mkdir(parents=True)
        shutil.copyfile(old, lib / 'libkemu_test.so')
        os.chmod(lib / 'libkemu_test.so', 0o644)
        if name == 'system':
            for filename in regular:
                target = tree / 'system' / filename
                target.write_bytes(b'original content')
                target.chmod(0o755 if filename == 'program' else 0o640)
            (tree / 'system/link').symlink_to('config.txt')
            (tree / 'system/hard').write_bytes(b'hardlinked')
            os.link(tree / 'system/hard', tree / 'system/hard2')
        image = work / (name + '.img')
        run([host / 'mke2fs', '-q', '-F', '-t', 'ext4', '-b', '4096', '-O', '^has_journal',
             '-d', tree, image, '4096'])
        guest_path = '/system/lib64/libkemu_test.so' if name == 'system' else '/lib64/libkemu_test.so'
        for field, value in [('uid', '1234'), ('gid', '2345'), ('mode', '0100640')]:
            run([host / 'debugfs', '-w', '-R', f'set_inode_field {guest_path} {field} {value}', image])
        run([host / 'debugfs', '-w', '-R', f'ea_set {guest_path} security.selinux u:object_r:system_file:s0', image])
        run([host / 'debugfs', '-w', '-R', f'ea_set {guest_path} user.kemu sentinel', image])
        command = [avb, 'add_hashtree_footer', '--image', image, '--partition_name', name,
                   '--partition_size', str(32 << 20), '--hash_algorithm', 'sha256' if name == 'system' else 'sha1',
                   '--salt', '0123456789abcdef']
        if name == 'system':
            command += ['--algorithm', 'SHA256_RSA2048', '--key', keys / 'testkey_rsa2048.pem']
        run(command, env=env)
    run([avb, 'extract_public_key', '--key', keys / 'testkey_rsa2048.pem', '--output', work / 'system.pub'])
    run([avb, 'make_vbmeta_image', '--algorithm', 'SHA256_RSA4096', '--key', keys / 'testkey_rsa4096.pem',
         '--chain_partition', 'system:1:' + str(work / 'system.pub'),
         '--include_descriptors_from_image', work / 'vendor.img', '--output', work / 'vbmeta.img'])
    command = [host / 'lpmake', '--metadata-size', '65536', '--metadata-slots', '2', '--super-name', 'super',
               '--device', 'super:' + str(80 << 20), '--group', 'kemu_dynamic:' + str(72 << 20)]
    for name in ['system', 'vendor']:
        command += ['--partition', f'{name}:readonly:{32 << 20}:kemu_dynamic',
                    '--image', name + '=' + str(work / (name + '.img'))]
    run(command + ['--output', work / 'super.img'])
    base = work / 'base.img'; pack_gpt(base, work / 'super.img', work / 'vbmeta.img')
    base_hash = sha(base)
    manifest = {'schema_version': 1, 'base_image_sha256': base_hash, 'files': [
        {'path': '/system/lib64/libkemu_test.so', 'source': 'large.so', 'sha256': sha(payload / 'large.so')},
        {'path': '/vendor/lib64/libkemu_test.so', 'source': 'small.so', 'sha256': sha(old)}]}
    manifest['files'] += [{'path': '/system/' + name, 'source': name, 'sha256': sha(payload / name)} for name in regular]
    job = work / 'job.json'; job.write_text(json.dumps(manifest))
    common = [args.binary.resolve(), '--input', base, '--manifest', job, '--payload-dir', payload,
              '--tools-dir', host]
    rejected = run(common + ['--key', 'vbmeta=unused.pem'], expected=(1,), env=env)
    assert 'unknown option: --key' in rejected
    assert '--key' not in run([args.binary.resolve(), '--help'])
    output = work / 'output'
    text = run(common + ['--output-dir', output], env=env)
    (work / 'burn.log').write_text(text)
    result = json.loads((output / 'result.json').read_text())
    assert result['base_image_sha256'] == base_hash
    assert sha(base) == base_hash and result['image_sha256'] == sha(output / 'system.img')
    extracted = work / 'extracted'
    named, layout = unpack(output / 'system.img', extracted, host / 'lpdump')
    run([avb, 'verify_image', '--image', extracted / 'vbmeta.img', '--follow_chain_partitions'], env=env)
    digest = run([avb, 'calculate_vbmeta_digest', '--image', extracted / 'vbmeta.img']).strip()
    assert digest == result['vbmeta_digest'] and digest in (output / 'VerifiedBootParams.textproto').read_text()
    for name in ['system', 'vendor']:
        image = extracted / (name + '.img')
        guest = '/system/lib64/libkemu_test.so' if name == 'system' else '/lib64/libkemu_test.so'
        dump = extracted / (name + '.so')
        run([host / 'debugfs', '-R', f'dump {guest} {dump}', image])
        assert sha(dump) == sha(payload / ('large.so' if name == 'system' else 'small.so'))
        stat = run([host / 'debugfs', '-R', f'stat {guest}', image])
        import re
        assert re.search(r'User:\s+1234', stat) and re.search(r'Group:\s+2345', stat)
        assert re.search(r'Mode:\s+0640', stat)
        attrs = run([host / 'debugfs', '-R', f'ea_list {guest}', image])
        assert 'u:object_r:system_file:s0' in attrs and 'sentinel' in attrs

        with image.open('rb') as f:
            f.seek(-64, 2); footer = f.read(64)
            _, vbmeta_offset, _ = struct.unpack_from('>QQQ', footer, 12)
            f.seek(vbmeta_offset); header = f.read(256)
            auth = struct.unpack_from('>Q', header, 12)[0]
            desc_offset = struct.unpack_from('>Q', header, 96)[0]
            f.seek(vbmeta_offset + 256 + auth + desc_offset); desc = f.read(180)
            assert struct.unpack_from('>Q', desc, 0)[0] == 1
            fec_offset, fec_size = struct.unpack_from('>QQ', desc, 56)
            roots = struct.unpack_from('>I', desc, 52)[0]
            f.seek(0)
            fec_input = extracted / (name + '.fec-input')
            with fec_input.open('wb') as target:
                target.write(f.read(fec_offset))
            f.seek(fec_offset); actual_fec = f.read(fec_size)
        expected_fec = extracted / (name + '.fec')
        run([host / 'fec', '--encode', '--roots', str(roots), fec_input, expected_fec])
        assert expected_fec.read_bytes()[:fec_size] == actual_fec, 'FEC differs from AOSP reference'
        assert image.stat().st_size != 32 << 20, 'test must exercise partition size changes'
    for name, data in regular.items():
        dumped = extracted / ('regular-' + name)
        run([host / 'debugfs', '-R', f'dump /system/{name} {dumped}', extracted / 'system.img'])
        assert dumped.read_bytes() == data
        stat = run([host / 'debugfs', '-R', f'stat /system/{name}', extracted / 'system.img'])
        assert re.search(r'Mode:\s+' + ('0755' if name == 'program' else '0640'), stat)
    for target in ['missing', 'lib64', 'link', 'hard']:
        bad = dict(manifest, files=[{'path': '/system/' + target, 'source': 'config.txt', 'sha256': sha(payload / 'config.txt')}])
        job.write_text(json.dumps(bad))
        rejected_output = work / ('reject-' + target)
        run(common + ['--output-dir', rejected_output], expected=(1,), env=env)
        assert not (rejected_output / 'result.json').exists()
        assert sha(base) == base_hash
    job.write_text(json.dumps(manifest))
    # A failing editor must stop the transaction before it can be marked ready.
    failing_tools = work / 'failing-tools'; failing_tools.mkdir()
    for name in ['e2fsck', 'resize2fs']:
        (failing_tools / name).symlink_to(host / name)
    helper = failing_tools / 'kemu_ext4_tool'
    helper.write_text('#!/bin/sh\nexit 23\n'); helper.chmod(0o755)
    failing_command = list(common)
    failing_command[failing_command.index('--tools-dir') + 1] = failing_tools
    failed_output = work / 'failed-editor'
    run(failing_command + ['--output-dir', failed_output], expected=(1,), env=env)
    assert not (failed_output / 'result.json').exists()
    # A successful helper exit is insufficient when the extracted bytes are wrong.
    helper.write_text('#!/usr/bin/python3\nimport subprocess,sys\n'
                      + 'code = subprocess.call([' + repr(str(host / 'kemu_ext4_tool')) + '] + sys.argv[1:])\n'
                      + "if code == 0 and sys.argv[1] == 'extract':\n"
                      + "    with open(sys.argv[4], 'ab') as f: f.write(b'corrupt-readback')\n"
                      + 'sys.exit(code)\n')
    corrupted_output = work / 'corrupted-readback'
    message = run(failing_command + ['--output-dir', corrupted_output], expected=(1,), env=env)
    assert 'replacement readback hash mismatch' in message
    assert not (corrupted_output / 'result.json').exists()

    # Input hashes are mandatory and must match before an output transaction exists.
    for label, value in [('missing', None), ('invalid', 'xyz'), ('wrong-type', 123), ('mismatch', '0' * 64)]:
        bad = dict(manifest)
        if value is None:
            del bad['base_image_sha256']
        else:
            bad['base_image_sha256'] = value
        job.write_text(json.dumps(bad))
        failure = work / ('input-hash-' + label)
        message = run(common + ['--output-dir', failure], expected=(1,), env=env)
        assert ('base image hash mismatch' if label == 'mismatch' else 'base_image_sha256') in message
        assert not failure.exists()
    # Hash mismatch must fail before creating the output transaction.
    bad = json.loads(json.dumps(manifest)); bad['files'][0]['sha256'] = '0' * 64
    job.write_text(json.dumps(bad)); failure = work / 'bad-hash'
    run(common + ['--output-dir', failure], expected=(1,), env=env)
    assert not failure.exists()
    # Traversal is rejected before invoking filesystem tools.
    bad['files'][0]['path'] = '/system/../vendor/libkemu_test.so'
    bad['files'][0]['sha256'] = sha(payload / 'large.so'); job.write_text(json.dumps(bad))
    run(common + ['--output-dir', work / 'bad-path'], expected=(1,), env=env)
    assert not (work / 'bad-path').exists()
    job.write_text(json.dumps(manifest))
    assert sha(base) == base_hash
    (work / 'verification.json').write_text(json.dumps({'status': 'passed', 'reference_avb_verified': True,
        'reference_fec_matches': True, 'input_unchanged': True, 'growth_and_relayout': True,
        'payload_reconstruction': True, 'regular_and_empty_files': True, 'non_regular_and_missing_rejected': True, 'hash_and_traversal_rejected': True}, indent=2))
    print('PASS: AVB signatures, chained digest, reference FEC, replacement bytes, changed partition sizes, input preservation and rejection tests')


if __name__ == '__main__':
    main()
