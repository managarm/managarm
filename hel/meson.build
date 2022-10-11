headers = [
	'include/hel.h',
	'include/hel-stubs.h',
	'include/hel-syscalls.h',
	'include/hel-types.h',
	'include/helix/ipc.hpp',
	'include/helix/memory.hpp'
]

deps = [ coroutines, bragi_dep, frigg ]
inc = [ 'include' ]

helix = shared_library('helix', 'src/globals.cpp',
	dependencies : deps,
	include_directories : inc,
	install : true
)

helix_dep = declare_dependency(
	link_with : helix,
	dependencies : deps,
	include_directories : inc
)

install_headers(headers)
