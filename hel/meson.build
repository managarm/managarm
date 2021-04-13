if get_option('build_drivers')
	helix = shared_library('helix', 'src/globals.cpp',
		dependencies: [coroutines, bragi_dep],
		include_directories: [include_directories('include/')],
		install: true)

	install_headers(
		'include/hel.h',
		'include/hel-stubs.h',
		'include/hel-syscalls.h')

	install_headers(
		'include/helix/ipc.hpp',
		'include/helix/memory.hpp')

	lib_helix_dep = declare_dependency(
		dependencies: [bragi_dep],
		include_directories: [include_directories('include/')],
		link_with: helix)
endif

doxyfile = configure_file(
	input: 'Doxyfile.in',
	output: 'Doxyfile',
	configuration: {
		'ROOTDIR': meson.current_source_dir()
	}
)

doxygen = find_program('doxygen', required: false)
if doxygen.found()
	custom_target('hel-doxygen',
		input: doxyfile,
		output: 'doc',
		depend_files: 'include/hel.h',
		command: [doxygen, doxyfile]
	)
endif
