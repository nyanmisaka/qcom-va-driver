# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Qualcomm Technologies, Inc. and/or its subsidiaries

executable(
  'api-smoke',
  'api-smoke.cpp',
  dependencies: [va, va_drm],
  install: true,
  install_dir: tests_install_dir / 'encoder',
)

executable(
  'async',
  'async.cpp',
  dependencies: [va, va_drm, threads],
  install: true,
  install_dir: tests_install_dir / 'encoder',
)

if have_gpu_copy
  executable(
    'writable-export',
    'writable-export.cpp',
    dependencies: [
      dependency('libavcodec'),
      dependency('libavutil'),
      va,
      drm,
      gbm,
      egl,
      gles,
    ],
    install: true,
    install_dir: tests_install_dir / 'encoder',
  )
endif

shared_module(
  'reject-coherent',
  'reject-coherent.c',
  dependencies: [drm, dl],
  name_prefix: '',
  install: true,
  install_dir: tests_install_dir / 'encoder',
)

shared_module(
  'delay-completion',
  'delay-completion.c',
  dependencies: [dl],
  name_prefix: '',
  install: true,
  install_dir: tests_install_dir / 'encoder',
)

install_data(
  'ffmpeg-smoke.py',
  install_dir: tests_install_dir / 'encoder',
  install_mode: 'rwxr-xr-x',
)
