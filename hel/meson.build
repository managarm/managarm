headers = [
	'include/hel.h',
	'include/hel-stubs.h',
	'include/hel-syscalls.h',
	'include/hel-types.h',
	'include/helix/ipc.hpp',
	'include/helix/memory.hpp'
]

if arch == 'aarch64'
	headers += 'include/hel-stubs-aarch64.h'
elif arch == 'x86_64'
	headers += 'include/hel-stubs-x86_64.h'
endif

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
