project('qaic', version: '01.00.51', license: 'BSD-3-Clause-Clear')
python = import('python').find_installation()
ghc = find_program('ghc', native: true)
qaic = custom_target('qaic', output: 'qaic',
  command: [python, files('build.py'), ghc, meson.current_source_dir(), '@OUTPUT@'],
  depend_files: files('build.py'), build_by_default: false)
qaic_idl_dir = meson.current_source_dir() / 'inc'
install_data('LICENSE.txt', install_dir: get_option('datadir') / 'licenses/qcom-va-driver/qaic')
