project(
  'qcom-va-driver', 'cpp', 'c',
  version: '1.1.0',
  meson_version: '>=0.61.0',
  license: 'MIT',
  default_options: ['cpp_std=c++17', 'warning_level=2'],
)

va = dependency('libva', version: '>=1.10')
drm = dependency('libdrm')
threads = dependency('threads')
gpu_copy_option = get_option('gpu_copy')
egl = dependency('egl', required: gpu_copy_option)
gles = dependency('glesv2', version: '>=3.2', required: gpu_copy_option)
gbm = dependency('gbm', required: gpu_copy_option)
have_gpu_copy = egl.found() and gles.found() and gbm.found()
copy_dependencies = []
copy_source = files('src/cpu-copy.cpp')
if have_gpu_copy
  copy_dependencies = [egl, gles, gbm]
  copy_source = files('src/gpu-copy.cpp')
endif
summary('GPU surface copy', have_gpu_copy)

vpp_args = []
vpp_dependencies = []
cdsp_option = get_option('cdsp_vpp')
have_cdsp_vpp = false
if not cdsp_option.disabled()
  supported_build = build_machine.system() == 'linux' and build_machine.cpu_family() == 'aarch64'
  if cdsp_option.enabled() and not supported_build
    error('cdsp_vpp requires a Linux ARM64 build machine (Open Access 19.0.02)')
  endif
  if supported_build
    have_cdsp_vpp = find_program('ghc', native: true, required: cdsp_option).found()
  endif
endif
if have_cdsp_vpp
  subdir('src/cdsp')
  vpp_args = ['-DHAVE_CDSP_VPP', '-DIRIS_VPP_CLIENT_PATH="' +
              (get_option('prefix') / get_option('libdir') / 'qcom-va-driver/libiris_vpp.so') + '"']
  vpp_dependencies = [meson.get_compiler('cpp').find_library('dl')]
endif
summary('CDSP VPP', have_cdsp_vpp)

vulkan_option = get_option('vulkan_vpp')
vulkan = dependency('vulkan', version: '>=1.3', required: vulkan_option)
glslang = find_program('glslangValidator', native: true, required: vulkan_option)
have_vulkan_vpp = not vulkan_option.disabled() and vulkan.found() and glslang.found()
vulkan_sources = []
if have_vulkan_vpp
  subdir('src/vulkan')
  vpp_args += ['-DHAVE_VULKAN_VPP']
  vpp_dependencies += [vulkan]
endif
summary('Vulkan VPP', have_vulkan_vpp)
vpp_backend = get_option('vpp_backend')
if (vpp_backend == 'cdsp' and not have_cdsp_vpp) or (vpp_backend == 'vulkan' and not have_vulkan_vpp)
  error('The selected vpp_backend must be enabled at build time')
endif
vpp_args += ['-DIRIS_VPP_DEFAULT_BACKEND="' + vpp_backend + '"']

sources = files(
  'src/backend.cpp',
  'src/av1.cpp',
  'src/decoder.cpp',
  'src/encoder.cpp',
  'src/h264.cpp',
  'src/hevc.cpp',
  'src/msm.c',
  'src/surface-copy.cpp',
  'src/vp9.cpp',
  'src/vpp.cpp',
)
headers = files('src/internal.hpp')
msm_allocation_test = executable('msm-allocation-test',
  'tests/vpp/msm-allocation.c', 'src/msm.c',
  dependencies: drm, c_args: ['-UNDEBUG'], build_by_default: false)
test('MSM allocation cache modes', msm_allocation_test)
capture_pool_test = executable('capture-pool-test', 'tests/decoder/capture-pool.cpp',
  dependencies: [va, drm, threads], cpp_args: ['-UNDEBUG'], build_by_default: false)
test('Decoder CAPTURE memory budget', capture_pool_test)
driver_install_dir = get_option('libdir') / 'dri'

shared_library(
  'v4l2_drv_video',
  sources + copy_source + vulkan_sources,
  dependencies: [va, drm, threads] + copy_dependencies + vpp_dependencies,
  cpp_args: vpp_args,
  name_prefix: '',
  install: true,
  install_dir: driver_install_dir,
)

# libva falls back to the DRM driver name on Qualcomm MSM devices.
install_symlink(
  'msm_drv_video.so',
  pointing_to: 'v4l2_drv_video.so',
  install_dir: driver_install_dir,
  install_tag: 'runtime',
)

install_data('README.md', install_dir: get_option('datadir') / 'doc' / meson.project_name())
install_data('LICENSE', install_dir: get_option('datadir') / 'licenses' / meson.project_name())

formatter = find_program('clang-format', required: false)
if formatter.found()
  sources += files('src/cpu-copy.cpp', 'src/gpu-copy.cpp', 'src/cdsp/client.cpp',
                   'src/cdsp/dsp.c', 'src/cdsp/filter.h', 'src/cdsp/client.h',
                   'src/overlay-params.h', 'src/cdsp/overlay.h',
                   'src/cdsp/qurt_abi.h', 'src/cdsp/qurt_abi_checks.h',
                   'src/cdsp/skel.cpp', 'src/cdsp/skel.h', 'src/cdsp/vpp_limits.h')
  sources += files('src/vulkan/vpp.cpp', 'src/vulkan/vpp.hpp')
  run_target('format', command: [formatter, '-i', '--style=file', sources, headers])
  run_target('format-check', command: [formatter, '--dry-run', '--Werror', '--style=file', sources, headers])
endif
