if host_machine.cpu_family() not in ['aarch64']
	subdir_done()
endif

eir_linux_sources = files(
	host_machine.cpu_family() + '.S',
	'entry.cpp',
)

if host_machine.cpu_family() == 'aarch64'
	eir_linux_sources += '../../generic/generic-platform.cpp'
endif

objcopy = find_program('aarch64-managarm-objcopy')

exe = executable('eir-linux', eir_sources, eir_linux_sources,
	'../../generic/elf-relocate.cpp',
	include_directories : eir_includes,
	cpp_args : [eir_cpp_args, '-DEIR_PIE'],
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x', '-static-pie'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	pie : true,
	install : true
)

rawbin = custom_target('eir-linux.bin',
	command : [ objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@' ],
	input : exe,
	output : 'eir-linux.bin',
	build_by_default : true,
	install : true,
	install_dir : get_option('bindir'),
)
