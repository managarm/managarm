mbus_bragi = cxxbragi.process('mbus.bragi')

src = [ 'src/client.cpp', 'src/client_ng.cpp', mbus_bragi ]
inc = [ 'include' ]
deps = [ helix_dep, posix_extra_dep, frigg ]

libmbus = shared_library('mbus', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

mbus_proto_dep = declare_dependency(
	link_with : libmbus,
	dependencies : deps,
	sources : mbus_bragi,
	include_directories : inc
)

install_headers('include/protocols/mbus/client.hpp',
	subdir : 'protocols/mbus'
)
