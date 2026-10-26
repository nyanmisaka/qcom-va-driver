#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a local userspace makepkg source archive from the current working tree."""
import argparse
import gzip
import io
from pathlib import Path
import re
import tarfile


def archive(destination, entries):
    # Fixed metadata makes archives independent of checkout timestamps/owners.
    with destination.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode='w') as tar:
            for name, path in sorted(entries):
                data = path.read_bytes()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o644
                tar.addfile(info, io.BytesIO(data))
    print(destination)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    version = re.search(r"version:\s*'([^']+)'", (root / 'meson.build').read_text())[1]
    name = 'qcom-va-driver-' + version
    paths = [root / p for p in ('meson.build', 'meson_options.txt', 'LICENSE', 'README.md', '.clang-format', '.editorconfig')]
    for directory in ('src', 'patches', 'tests'):
        paths += [p for p in (root / directory).rglob('*') if p.is_file()
                  and '__pycache__' not in p.parts and p.suffix != '.pyc']
    paths += list((root / 'subprojects').glob('*.wrap'))
    paths += [p for p in (root / 'subprojects/packagefiles').rglob('*') if p.is_file()
              and '__pycache__' not in p.parts]
    archive(root / 'packaging/arch' / (name + '.tar.gz'), [(name + '/' + str(p.relative_to(root)), p) for p in paths])


if __name__ == '__main__':
    main()
