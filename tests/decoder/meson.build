# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Qualcomm Technologies, Inc. and/or its subsidiaries

executable(
  'main10',
  'main10.cpp',
  dependencies: [va, va_drm],
  install: true,
  install_dir: tests_install_dir / 'decoder',
)

executable(
  'prime-import',
  'prime-import.cpp',
  dependencies: [va, va_drm],
  install: true,
  install_dir: tests_install_dir / 'decoder',
)

# Prebuilt shims and helpers for error-recovery.py and av1.py, so the
# scripts do not need a compiler at runtime.
shared_module(
  'error-completion',
  'error-completion.c',
  dependencies: [dl],
  name_prefix: '',
  install: true,
  install_dir: tests_install_dir / 'decoder',
)

shared_module(
  'surface-storage',
  'surface-storage.cpp',
  dependencies: [va, dl, threads],
  name_prefix: '',
  install: true,
  install_dir: tests_install_dir / 'decoder',
)

gst_app = dependency('gstreamer-app-1.0', required: false)
gst_video = dependency('gstreamer-video-1.0', required: false)
if gst_app.found() and gst_video.found()
  executable(
    'av1-download',
    'av1-download.cpp',
    dependencies: [gst_app, gst_video],
    install: true,
    install_dir: tests_install_dir / 'decoder',
  )
endif

install_data(
  'av1.py',
  'error-recovery.py',
  install_dir: tests_install_dir / 'decoder',
  install_mode: 'rwxr-xr-x',
)
