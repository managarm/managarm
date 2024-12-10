objcopy = find_program('riscv64-managarm-objcopy')

exe = executable('eir-virt',
	[
		eir_sources,
		'../../entry64.S',
		'../../../../generic/elf-relocate.cpp',
		'early-log.cpp',
	],
	include_directories : eir_includes,
	c_args : ['-DEIR_PIE'], # Assembly is treated as C.
	cpp_args : eir_cpp_args + ['-DEIR_PIE'],
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x', '-static-pie'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	pie : true,
	install : true,
)

rawbin = custom_target('eir-virt.bin',
	command : [ objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@' ],
	input : exe,
	output : 'eir-virt.bin',
	build_by_default : true,
	install : true,
	install_dir : get_option('bindir'),
)
