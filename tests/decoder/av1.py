#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""AV1 VAAPI decoding against dav1d; FFmpeg and optionally GStreamer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

CASES = [
    ('720p8', 1280, 720, 8, 60, 0),
    ('1080p8', 1920, 1080, 8, 60, 0),
    ('1080p10', 1920, 1080, 10, 60, 0),
    ('4k8', 3840, 2160, 8, 30, 0),
    ('4k10', 3840, 2160, 10, 30, 0),
    ('1080p8-lookahead', 1920, 1080, 8, 60, 25),
    ('1080p10-lookahead', 1920, 1080, 10, 60, 25),
]
EXTENDED_CASES = [
    ('tiles8', 1280, 720, 8, 30, 25),
    ('tiles10', 1280, 720, 10, 30, 25),
    ('lossless8', 1280, 720, 8, 12, 0),
    ('still10', 1280, 720, 10, 1, 0),
    ('superres8', 1280, 720, 8, 12, 0),
    ('motion8', 1280, 720, 8, 30, 0),
]
ENCODER_OPTIONS = {
    'tiles8': ['-tiles', '2x2'],
    'tiles10': ['-tiles', '2x2'],
    'lossless8': ['-aom-params', 'lossless=1'],
    'still10': ['-still-picture', '1'],
    'motion8': ['-cpu-used', '4', '-enable-global-motion', '1', '-aq-mode', '1'],
}


def digest(path):
    with path.open('rb') as file:
        result = hashlib.sha256()
        for chunk in iter(lambda: file.read(1024 * 1024), b''):
            result.update(chunk)
        return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--driver-dir', type=Path, required=True)
    parser.add_argument('--samples-dir', type=Path, help='reuse existing av1-*.ivf and reference-* files')
    parser.add_argument('--gstreamer', action='store_true')
    parser.add_argument('--extended', action='store_true', help='also test individual coding tools')
    args = parser.parse_args()
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=True)
    samples = args.samples_dir.resolve() if args.samples_dir else work
    env = os.environ.copy()
    for key in list(env):
        if key == 'LD_PRELOAD' or key.startswith('V4L2_TEST_'):
            del env[key]
    env.update(LIBVA_DRIVER_NAME='qcom', LIBVA_DRIVERS_PATH=str(args.driver_dir.resolve()),
               IRIS_VAAPI_DEBUG='1', GST_VA_ALL_DRIVERS='1', GST_DEBUG='2',
               GST_DEBUG_NO_COLOR='1', GST_REGISTRY=str(work / 'registry.bin'))

    def run(command, log, timeout=120):
        with log.open('w') as file:
            result = subprocess.run(command, stdout=file, stderr=subprocess.STDOUT,
                                    env=env, timeout=timeout)
        if result.returncode:
            raise RuntimeError(f'{log.name}: exit {result.returncode}\n{log.read_text()[-6000:]}')

    # Installed copies ship a prebuilt helper; a source checkout compiles it.
    download = Path(__file__).with_name('av1-download')
    if args.gstreamer and not download.exists():
        download = work / 'av1-download'
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'gstreamer-app-1.0', 'gstreamer-video-1.0'], text=True))
        run(shlex.split(os.environ.get('CXX', 'c++')) + ['-O2', '-Wall', '-Wextra',
            str(Path(__file__).with_name('av1-download.cpp')), '-o', str(download)] + flags,
            work / 'build-download.log')
    results = []
    cases = CASES + (EXTENDED_CASES if args.extended else [])
    for name, width, height, depth, frames, lag in cases:
        raw, ffraw = ('nv12', 'nv12') if depth == 8 else ('p010', 'p010le')
        video = samples / f'av1-{name}.ivf'
        reference = samples / f'reference-{name}.{raw}'
        if not args.samples_dir:
            pixel = 'yuv420p' if depth == 8 else 'yuv420p10le'
            pattern = f'testsrc2=size={width}x{height}:rate=30,format={pixel}'
            if name == 'motion8':
                pattern = "testsrc2=size=1408x800:rate=30,crop=1280:720:x='mod(n*4,128)':y=0"
            source = ['ffmpeg', '-hide_banner', '-v', 'error', '-f', 'lavfi', '-i',
                      pattern, '-frames:v', str(frames)]
            if name == 'superres8':
                # Super-resolution config fields are not exposed by FFmpeg's libaom wrapper.
                y4m = work / 'superres8.y4m'
                run(source + ['-f', 'yuv4mpegpipe', '-y', str(y4m)], work / 'generate-y4m.log')
                run(['aomenc', '--cpu-used=8', '--end-usage=q', '--cq-level=30', '--threads=4',
                     '--row-mt=1', '--passes=1', '--lag-in-frames=0', '--superres-mode=1',
                     '--superres-denominator=12', '--superres-kf-denominator=12', '--ivf',
                     '-o', str(video), str(y4m)], work / f'generate-{name}.log')
                y4m.unlink()
            else:
                usage = 'good' if lag or name in ENCODER_OPTIONS else 'realtime'
                run(source + ['-c:v', 'libaom-av1', '-usage', usage, '-cpu-used', '8',
                     '-crf', '30', '-b:v', '0', '-g', '30', '-lag-in-frames', str(lag),
                     '-row-mt', '1', '-threads', '4'] + ENCODER_OPTIONS.get(name, []) +
                    ['-f', 'ivf', '-y', str(video)], work / f'generate-{name}.log')
            run(['ffmpeg', '-hide_banner', '-v', 'error', '-c:v', 'libdav1d', '-i', str(video),
                 '-pix_fmt', ffraw, '-f', 'rawvideo', '-y', str(reference)], work / f'reference-{name}.log')
        expected = digest(reference)
        for backend in ['ffmpeg'] + (['gstreamer'] if args.gstreamer else []):
            output = work / f'{backend}-{name}.{raw}'
            log = work / f'{backend}-{name}.log'
            env['IRIS_VAAPI_DUMP'] = str(work / f'{backend}-{name}.obu')
            if backend == 'ffmpeg':
                command = ['ffmpeg', '-hide_banner', '-v', 'verbose', '-hwaccel', 'vaapi',
                           '-hwaccel_device', '/dev/dri/renderD128', '-hwaccel_output_format', 'vaapi',
                           '-c:v', 'av1', '-i', str(video), '-vf', f'hwdownload,format={ffraw}',
                           '-fps_mode', 'passthrough', '-f', 'rawvideo', '-y', str(output)]
            else:
                fmt = 'NV12' if depth == 8 else 'P010_10LE'
                location = json.dumps(str(video))
                pipeline = (f'filesrc location={location} ! ivfparse ! av1parse ! vaav1dec ! '
                            f'video/x-raw,format={fmt} ! appsink name=output sync=false')
                command = [str(download), pipeline, str(output)]
            run(command, log, timeout=30)
            actual = digest(output)
            expected_size = width * height * 3 // 2 * (2 if depth == 10 else 1) * frames
            if actual != expected or output.stat().st_size != expected_size:
                raise RuntimeError(f'{backend} {name}: output does not match dav1d')
            text = log.read_text()
            if re.search(r'recover|VA status 0x|surface sync timeout', text):
                raise RuntimeError(f'{backend} {name}: driver reported a decoding failure')
            row = dict(case=name, backend=backend, frames=frames, pixel_identical=True, sha256=actual,
                       reference_replays=text.count('AV1 install reference'),
                       hidden_pictures=text.count('original-show=0'))
            results.append(row)
            (work / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            output.unlink()
            print(json.dumps(row), flush=True)
    print(f'PASS: {len(results)} AV1 cases, {sum(r["frames"] for r in results)} displayed frames')


if __name__ == '__main__':
    main()
