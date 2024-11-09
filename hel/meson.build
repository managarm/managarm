headers = [
	'include/hel.h',
	'include/hel-stubs.h',
	'include/hel-syscalls.h',
	'include/hel-types.h',
	'include/helix/ipc.hpp',
	'include/helix/memory.hpp',
	'include/helix/passthrough-fd.hpp'
]

src = files(
	'src/globals.cpp',
	'src/passthrough-fd.cpp',
)

if arch == 'aarch64'
	headers += 'include/hel-stubs-aarch64.h'
elif arch == 'riscv64'
	headers += 'include/hel-stubs-riscv64.h'
elif arch == 'x86_64'
	headers += 'include/hel-stubs-x86_64.h'
endif

inc = [ 'include' ]

if not provide_deps and not build_kernel
	deps = [ bragi_dep, frigg, posix_extra_dep ]

	helix = shared_library('helix', src,
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
endif

hel_dep = declare_dependency(
	include_directories : inc
)
