ostrace_bragi = cxxbragi.process('ostrace.bragi')

src = [ 'src/ostrace.cpp', ostrace_bragi ]
inc = [ 'include' ]
deps = [ mbus_dep, bragi_dep ]

libostrace_protocol = shared_library('ostrace_protocol', src,
	dependencies: deps,
	include_directories: inc,
	install: true
)

install_headers('include/protocols/ostrace/ostrace.hpp',
	subdir: 'protocols/ostrace'
)

ostrace_dep = declare_dependency(
	link_with: libostrace_protocol,
	dependencies : deps,
	sources : ostrace_bragi,
	include_directories: inc
)
