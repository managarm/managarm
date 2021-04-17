here = meson.current_source_dir()
svrctl_pb = protocxx.process(here/'svrctl.proto', 
	extra_args : [ '-I' + here ]
)

src = [ 'src/server.cpp', svrctl_pb ]
inc = [ 'include' ]
deps = [ helix_dep, proto_lite_dep ]

libsvrctl = shared_library('svrctl_protocol', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

svrctl_dep = declare_dependency(
	link_with : libsvrctl,
	dependencies : deps,
	sources : svrctl_pb,
	include_directories : inc
)

install_headers('include/protocols/svrctl/server.hpp',
	subdir : 'protocols/svrctl'
)
