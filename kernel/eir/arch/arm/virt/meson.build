raspi4_sources = files('virt.S', 'virt.cpp')
objcopy = find_program('aarch64-managarm-objcopy')

exe = executable('eir-virt', eir_sources, raspi4_sources,
	include_directories : eir_includes,
	cpp_args : [eir_cpp_args, '-DPIE'],
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x', '-static-pie'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	pie : true,
	install : true
)

custom_target('eir-virt.bin',
	input : exe,
	output : 'eir-virt.bin',
	build_by_default : true,
	command : [ objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@' ]
)
