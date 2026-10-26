# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Qualcomm Technologies, Inc. and/or its subsidiaries

# Hardware tests; they should be ran on a supported device.

tests_install_dir = get_option('libexecdir') / meson.project_name() / 'tests'
va_drm = dependency('libva-drm')
dl = meson.get_compiler('c').find_library('dl', required: false)

subdir('decoder')
subdir('encoder')
subdir('vpp')
