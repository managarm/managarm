ostrace_bragi = cxxbragi.process('ostrace.bragi')

src = [ 'src/ostrace.cpp', ostrace_bragi ]
inc = [ 'include' ]

if build_drivers
	deps = [ mbus_proto_dep, bragi_dep, frigg ]

	libostrace_protocol = shared_library('ostrace_protocol', src,
		dependencies : deps,
		include_directories : inc,
		install : true
	)

	install_headers('include/protocols/ostrace/ostrace.hpp',
		subdir : 'protocols/ostrace'
	)

	ostrace_proto_dep = declare_dependency(
		link_with : libostrace_protocol,
		dependencies : deps,
		sources : ostrace_bragi,
		include_directories : inc
	)
endif
