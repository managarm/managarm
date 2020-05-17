
helix = shared_library('helix', 'src/globals.cpp',
	dependencies: [clang_coroutine_dep],
	include_directories: include_directories('include/'),
	cpp_args: ['-std=c++17', '-Wall'],
	install: true)

install_headers(
    'include/hel.h',
    'include/hel-stubs.h',
    'include/hel-syscalls.h')

install_headers(
	'include/helix/ipc.hpp',
	'include/helix/memory.hpp')

lib_helix_dep = declare_dependency(
	include_directories: include_directories('include/'),
	link_with: helix)

doxyfile = configure_file(
	input: 'Doxyfile.in',
	output: 'Doxyfile',
	configuration: {
		'ROOTDIR': meson.source_root() / 'hel'
	}
)

custom_target('hel-doxygen',
	input: doxyfile,
	output: 'doc',
	depend_files: 'include/hel.h',
	command: [find_program('doxygen', required: false), doxyfile]
)
