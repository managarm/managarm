raspi4_sources = files('raspi4.S', 'raspi4.cpp')
objcopy = find_program('aarch64-managarm-objcopy')

exe = executable('eir-raspi4', eir_sources, raspi4_sources,
	include_directories: eir_includes,
	cpp_args: eir_cpp_args,
	link_args: [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x'],
	dependencies: eir_dependencies,
	link_depends: files('link.x'),
	install: true)

custom_target('kernel8.img',
	input: exe,
	output: 'kernel8.img',
	build_by_default: true,
	command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'])
