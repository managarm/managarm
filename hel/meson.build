headers = [
	'include/hel.h',
	'include/hel-stubs.h',
	'include/hel-syscalls.h',
	'include/helix/ipc.hpp',
	'include/helix/memory.hpp'
]

deps = [ coroutines, bragi_dep ]
inc = [ 'include' ]

if get_option('build_drivers')
	helix = shared_library('helix', 'src/globals.cpp',
		dependencies: deps,
		include_directories: inc,
		install: true
	)

	helix_dep = declare_dependency(
		link_with : helix,
		dependencies : deps,
		include_directories : inc
	)

	install_headers(headers)
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
