here = meson.current_source_dir()
hw_pb = protocxx.process(here/'hw.proto', 
	extra_args : [ '-I' + here ]
)

incs = [ 'include' ]
deps = [ helix_dep, proto_lite_dep ]
src = [ 'src/client.cpp', hw_pb ]

libhw = shared_library('hw_protocol', src,
	dependencies : deps,
	include_directories : incs,
	install : true
)

hw_dep = declare_dependency(
	link_with : libhw,
	dependencies : deps,
	sources : hw_pb,
	include_directories : incs
)

install_headers('include/protocols/hw/client.hpp',
	subdir : 'protocols/hw'
)
