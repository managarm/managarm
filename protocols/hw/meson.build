hw_bragi = cxxbragi.process('hw.bragi')

incs = [ 'include' ]
deps = [ helix_dep, frigg ]
src = [ 'src/client.cpp', hw_bragi ]

libhw = shared_library('hw_protocol', src,
	dependencies : deps,
	include_directories : incs,
	install : true
)

hw_proto_dep = declare_dependency(
	link_with : libhw,
	dependencies : deps,
	sources : hw_bragi,
	include_directories : incs
)

install_headers('include/protocols/hw/client.hpp',
	subdir : 'protocols/hw'
)
