#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build-option regression; requires all GPU-copy development dependencies.

Usage: python3 tests/build/gpu-copy.py tmp/build-options
Masks each optional dependency in turn without changing installed packages.
"""
import argparse
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


GPU_PACKAGES = {'egl', 'glesv2', 'gbm'}


def pkg_config():
    requested = GPU_PACKAGES.intersection(
        word for argument in sys.argv[2:] for word in argument.split())
    with open(os.environ['GPU_COPY_TEST_QUERIES'], 'a') as log:
        log.write(' '.join(sorted(requested)) + '\n')
    if requested.intersection(os.environ['GPU_COPY_TEST_MISSING'].split(',')):
        return 1
    return subprocess.run([os.environ['GPU_COPY_TEST_PKG_CONFIG'], *sys.argv[2:]]).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='gpu-copy-', dir=args.directory.resolve()))
    source = Path(__file__).resolve().parents[2]
    pkg = shutil.which('pkg-config')
    subprocess.run([pkg, '--exists', 'libva >= 1.10', 'libdrm', 'egl',
                    'glesv2 >= 3.2', 'gbm'], check=True)
    native = root / 'native.ini'
    # Block CMake fallback as well, so hidden pkg-config packages stay unavailable.
    native.write_text('[binaries]\npkg-config = ' + repr([
        sys.executable, str(Path(__file__).resolve()), '--pkg-config']) +
        '\ncmake = ' + repr(shutil.which('false')) + '\n')
    ctypes.CDLL('libva.so.2', mode=os.RTLD_NOW | os.RTLD_GLOBAL)
    cases = [('disabled', ''), ('disabled', 'egl,glesv2,gbm'),
             ('auto', ''), ('enabled', '')]
    cases += [(mode, missing) for missing in ['egl', 'glesv2', 'gbm', 'egl,glesv2,gbm']
              for mode in ['auto', 'enabled']]
    for mode, missing in cases:
        name = mode + '-' + (missing.replace(',', '-') or 'available')
        build = root / name
        queries = root / (name + '.queries')
        env = dict(os.environ, GPU_COPY_TEST_QUERIES=str(queries),
                   GPU_COPY_TEST_MISSING=missing, GPU_COPY_TEST_PKG_CONFIG=pkg)
        with (root / (name + '.log')).open('w') as log:
            setup = subprocess.run(['meson', 'setup', str(build), str(source),
                                    '--native-file', str(native), '-Dgpu_copy=' + mode,
                                    '-Dcdsp_vpp=disabled', '-Dwerror=true'],
                                   env=env, stdout=log, stderr=subprocess.STDOUT)
            if mode == 'enabled' and missing:
                assert setup.returncode != 0, name
                assert missing.split(',')[0] in queries.read_text(), name
                print(f'PASS {name}: configuration rejected', flush=True)
                continue
            assert setup.returncode == 0, f'{name}: see {log.name}'
            subprocess.run(['meson', 'compile', '-C', str(build), '-j', '2'], env=env,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
        driver = build / 'qcom_drv_video.so'
        dynamic = subprocess.check_output(['readelf', '-d', str(driver)], text=True)
        needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
        gpu = mode != 'disabled' and not missing
        for library in ['libEGL.so', 'libGLESv2.so', 'libgbm.so']:
            assert any(n.startswith(library) for n in needed) == gpu, (name, needed)
        commands = (build / 'compile_commands.json').read_text()
        assert ('gpu-copy.cpp' in commands) == gpu, name
        assert ('cpu-copy.cpp' in commands) != gpu, name
        if mode == 'disabled':
            assert not queries.read_text().strip(), name
        module = ctypes.CDLL(str(driver), mode=os.RTLD_NOW)
        getattr(module, '__vaDriverInit_1_0')
        print(f'PASS {name}: build, dependencies, source selection, driver load', flush=True)
    print(f'Logs: {root}', flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--pkg-config':
        sys.exit(pkg_config())
    main()
