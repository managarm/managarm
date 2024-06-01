hw_bragi = cxxbragi.process('hw.bragi')

incs = [ 'include' ]
src = [ 'src/client.cpp', hw_bragi ]

if not provide_deps
	deps = [ helix_dep, frigg ]

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
endif

install_headers('include/protocols/hw/client.hpp',
	subdir : 'protocols/hw'
)
