here = meson.current_source_dir()
mbus_pb = protocxx.process(here/'mbus.proto', 
	extra_args : [ '-I' + here ]
)

src = [ 'src/client.cpp', mbus_pb ]
inc = [ 'include' ]
deps = [ helix_dep, proto_lite_dep ]

libmbus = shared_library('mbus', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

mbus_proto_dep = declare_dependency(
	link_with : libmbus,
	dependencies : deps,
	sources : mbus_pb,
	include_directories : inc
)

install_headers('include/protocols/mbus/client.hpp',
	subdir : 'protocols/mbus'
)
