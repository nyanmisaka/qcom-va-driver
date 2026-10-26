# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Qualcomm Technologies, Inc. and/or its subsidiaries

executable(
  'vpp-overlay',
  'overlay.cpp',
  dependencies: [va, va_drm],
  install: true,
  install_dir: tests_install_dir / 'vpp',
)

if have_vulkan_vpp
  executable(
    'vpp-tonemap',
    'tonemap.cpp',
    dependencies: [va, va_drm, drm, vulkan, dl],
    export_dynamic: true,
    install: true,
    install_dir: tests_install_dir / 'vpp',
  )

  # scaling.cpp always includes the Vulkan headers; the CDSP backend is
  # optional and compiled out with VPP_NO_CDSP.
  scaling_deps = [va, va_drm, drm, vulkan, dl]
  scaling_args = ['-DVPP_NO_CDSP']
  scaling_link = []
  scaling_rpath = ''
  if have_cdsp_vpp
    scaling_deps += [fastrpc_dep.partial_dependency(includes: true)]
    scaling_args = []
    scaling_link = [vpp_client]
    scaling_rpath = get_option('prefix') / get_option('libdir') / 'qcom-va-driver'
  endif
  executable(
    'vpp-scaling',
    'scaling.cpp',
    dependencies: scaling_deps,
    cpp_args: scaling_args,
    link_with: scaling_link,
    export_dynamic: true,
    install: true,
    install_dir: tests_install_dir / 'vpp',
    install_rpath: scaling_rpath,
  )
endif
