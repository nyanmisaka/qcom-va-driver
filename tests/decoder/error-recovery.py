#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Board regression: error completion, replacement pixels, and stable exports.

Usage: error-recovery.py tmp/error-recovery --driver-dir build
Requires FFmpeg with libx265, cc/c++, and the timestamp-preserving Iris kernel.
Builds the adjacent preload shims; all generated data stays in the output dir.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--driver-dir', type=Path, required=True)
    args = parser.parse_args()
    root = args.directory.resolve()
    root.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent
    error_shim = source / 'error-completion.so'
    storage_shim = source / 'surface-storage.so'
    # Installed copies ship prebuilt shims; a source checkout compiles them.
    if not error_shim.exists() or not storage_shim.exists():
        error_shim = root / 'error-completion.so'
        storage_shim = root / 'surface-storage.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-shared', '-fPIC',
                        str(source / 'error-completion.c'), '-ldl', '-o', str(error_shim)], check=True)
        subprocess.run(['c++', '-Wall', '-Wextra', '-shared', '-fPIC',
                        str(source / 'surface-storage.cpp'), '-ldl', '-pthread',
                        '-o', str(storage_shim)], check=True)
    env = dict(os.environ, LIBVA_DRIVER_NAME='qcom',
               LIBVA_DRIVERS_PATH=str(args.driver_dir.resolve()), IRIS_VAAPI_DEBUG='1')
    for key in list(env):
        if key.startswith('IRIS_TEST_') or key == 'LD_PRELOAD':
            del env[key]
    width, height, frames = 320, 240, 120
    frame_samples = width * height * 3 // 2

    def decode(name, sample, depth, queue='', first=46, count=1, storage='', drm=False,
               payload='', failure=''):
        runenv = env.copy()
        libraries = []
        if queue:
            libraries.append(str(error_shim))
            runenv.update(IRIS_TEST_ERROR_QUEUE=queue, IRIS_TEST_ERROR_FIRST=str(first),
                          IRIS_TEST_ERROR_COUNT=str(count), IRIS_TEST_ERROR_PAYLOAD=payload)
        if storage:
            libraries.append(str(storage_shim))
            runenv['IRIS_TEST_STORAGE'] = storage
        if libraries:
            runenv['LD_PRELOAD'] = ':'.join(libraries)
        if failure:
            runenv[failure] = '1'
        pixel_format = 'nv12' if depth == 8 else 'p010le'
        filters = ('hwmap=derive_device=drm:mode=read,format=drm_prime,hwmap=mode=read'
                   if drm else 'hwdownload')
        command = ['ffmpeg', '-hide_banner', '-nostdin', '-nostats',
                   '-init_hw_device', 'drm=drm:/dev/dri/renderD128',
                   '-init_hw_device', 'vaapi=va@drm', '-hwaccel', 'vaapi',
                   '-hwaccel_device', 'va', '-hwaccel_output_format', 'vaapi',
                   '-i', str(sample), '-vf', f'{filters},format={pixel_format}',
                   '-fps_mode', 'passthrough', '-f', 'rawvideo', '-']
        result = subprocess.run(command, env=runenv, capture_output=True, timeout=30)
        log = result.stderr.decode(errors='replace')
        (root / f'{name}.log').write_text(log)
        if failure:
            assert result.returncode != 0 and 'injected CAPTURE error' in log, name
            assert 'replaced damaged picture' not in log, name
            if failure == 'IRIS_TEST_FAIL_RECOVERY':
                assert 'timeout' not in log and 'completed damaged token=46 status=0x1' in log
            else:
                assert 'surface sync timeout' in log, name
            print(f'PASS {name}: failure reported without false recovery', flush=True)
            return [], log
        assert result.returncode == 0, f'{name}: {log[-3000:]}'
        assert 'timeout' not in log, name
        size = frame_samples * (1 if depth == 8 else 2)
        assert len(result.stdout) == size * frames, (name, len(result.stdout), size * frames)
        if storage:
            assert 'verified persistent surface=' in log, name
        if queue:
            assert 'injected ' in log, name
        images = [hashlib.sha256(result.stdout[i:i + size]).hexdigest()
                  for i in range(0, len(result.stdout), size)]
        return images, log

    for depth, bframes in [(8, 0), (10, 0), (8, 3)]:
        label = f'{depth}bit-b{bframes}'
        sample = root / f'{label}.hevc'
        generate = subprocess.run([
            'ffmpeg', '-hide_banner', '-nostdin', '-y', '-f', 'lavfi', '-i',
            f'testsrc2=size={width}x{height}:rate=30', '-frames:v', str(frames),
            '-pix_fmt', 'yuv420p' if depth == 8 else 'yuv420p10le', '-c:v', 'libx265',
            '-x265-params', f'pools=1:frame-threads=1:keyint=30:bframes={bframes}',
            str(sample)], capture_output=True, timeout=60)
        (root / f'{label}-generate.log').write_bytes(generate.stderr)
        assert generate.returncode == 0
        reference, _ = decode(label + '-clean', sample, depth)
        gray = hashlib.sha256((b'\x80' if depth == 8 else b'\x00\x80') * frame_samples).hexdigest()
        assert gray not in reference
        cases = [('capture', 46, 1, '', False, ''),
                 ('output', 46, 1, '', False, ''),
                 ('both', 46, 1, '', False, ''),
                 ('capture', 46, 3, '', False, ''),
                 ('capture', 1, 1, '', False, ''),
                 ('capture', 120, 1, '', False, ''),
                 ('capture', 46, 1, '', False, 'keep'),
                 ('capture', 46, 1, '', True, '')]
        if not bframes:
            cases += [('capture', 46, 3, storage, False, '')
                      for storage in ['export', 'prime1', 'prime2']]
        for number, (queue, first, count, storage, drm, payload) in enumerate(cases):
            name = f'{label}-{number}-{queue}-{storage or "local"}'
            images, log = decode(name, sample, depth, queue, first, count, storage, drm, payload)
            changed = [i for i, (a, b) in enumerate(zip(reference, images)) if a != b]
            assert len(changed) == count and all(images[i] == gray for i in changed), (name, changed)
            assert log.count('replaced damaged picture') == count, name
            print(f'PASS {name}: {frames} frames, {count} gray replacements', flush=True)
        if not bframes:
            data = sample.read_bytes()
            starts = list(re.finditer(b'\x00\x00\x00\x01|\x00\x00\x01', data))
            nals = [(m.end(), starts[i + 1].start() if i + 1 < len(starts) else len(data))
                    for i, m in enumerate(starts) if (data[m.end()] >> 1) & 63 < 32]
            start, end = nals[45]
            assert end - start > 64
            corrupt = root / f'{label}-truncated.hevc'
            corrupt.write_bytes(data[:start + 64] + data[end:])
            images, log = decode(label + '-truncated', corrupt, depth)
            assert gray in images and 'replaced damaged picture' in log
            print(f'PASS {label}-truncated: {frames} frames, firmware error recovered', flush=True)
        if depth == 8 and not bframes:
            for failure in ['IRIS_TEST_FAIL_RECOVERY', 'IRIS_TEST_ZERO_TIMESTAMP']:
                decode(label + '-' + failure, sample, depth, queue='capture', failure=failure)


if __name__ == '__main__':
    main()
