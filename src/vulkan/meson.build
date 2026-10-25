tonemap_spirv = custom_target(
  'tonemap_spirv',
  input: 'tonemap.comp',
  output: 'tonemap_spirv.h',
  command: [glslang, '-V', '--target-env', 'vulkan1.1', '--vn', 'tonemap_spirv',
            '-o', '@OUTPUT@', '@INPUT@'],
)
overlay_spirv = custom_target(
  'overlay_spirv', input: 'overlay.comp', output: 'overlay_spirv.h',
  command: [glslang, '-V', '--target-env', 'vulkan1.1', '--vn', 'overlay_spirv',
            '-o', '@OUTPUT@', '@INPUT@'],
)
transform_spirv = custom_target(
  'transform_spirv', input: 'transform.comp', output: 'transform_spirv.h',
  command: [glslang, '-V', '--target-env', 'vulkan1.1', '--vn', 'transform_spirv',
            '-o', '@OUTPUT@', '@INPUT@'],
)
vertical_spirv = custom_target(
  'vertical_spirv', input: 'transform.comp', output: 'vertical_spirv.h',
  command: [glslang, '-V', '--target-env', 'vulkan1.1', '-DVERTICAL_PASS=1',
            '--vn', 'vertical_spirv', '-o', '@OUTPUT@', '@INPUT@'],
)
vulkan_sources = [files('vpp.cpp'), tonemap_spirv, overlay_spirv, transform_spirv, vertical_spirv]
