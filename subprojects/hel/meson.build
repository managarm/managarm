project('hel')

hel_headers = [
	'include/hel.h',
	'include/hel-stubs.h',
	'include/hel-syscalls.h',
	'include/hel-types.h',
]

helix_headers = [
	'include/helix/ipc.hpp',
	'include/helix/ipc-structs.hpp',
	'include/helix/memory.hpp',
	'include/helix/timer.hpp'
]

arch = host_machine.cpu_family()

if arch == 'aarch64'
	hel_headers += 'include/hel-stubs-aarch64.h'
elif arch == 'x86_64'
	hel_headers += 'include/hel-stubs-x86_64.h'
endif

hel_dep = declare_dependency(
	include_directories : 'include'
)

meson.override_dependency('hel', hel_dep)

install_headers(hel_headers)
install_headers(helix_headers, subdir : 'helix')

pkg = import('pkgconfig')
pkg.generate(
	name : 'hel',
	description : 'Hel IPC headers'
)

doxygen = find_program('doxygen', required : get_option('build_docs'))

if doxygen.found()
	doxyfile = configure_file(
		input : 'docs/Doxyfile.in',
		output : 'Doxyfile',
		configuration : {
			'ROOTDIR': meson.current_source_dir()
		}
	)

	run_target('hel-doxygen', command : [ doxygen, doxyfile ])
endif
