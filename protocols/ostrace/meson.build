ostrace_bragi = cxxbragi.process('ostrace.bragi')

libostrace_protocol = shared_library('ostrace_protocol', [ 'src/ostrace.cpp', ostrace_bragi ],
	dependencies: [
		coroutines,
		lib_helix_dep,
		libmbus_protocol_dep,
		proto_lite_dep],
	include_directories: 'include',
	install: true
)

install_headers(
	'include/protocols/ostrace/ostrace.hpp',
	subdir: 'protocols/ostrace/')

libostrace_protocol_dep = declare_dependency(
	link_with: libostrace_protocol,
	include_directories: 'include'
)
