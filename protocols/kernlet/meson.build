here = meson.current_source_dir()
kernlet_pb = protocxx.process(here/'kernlet.proto', 
	extra_args : [ '-I' + here ]
)

deps = [ mbus_dep ]
src = [ 'src/compiler.cpp', kernlet_pb ]
inc = [ 'include' ]

libkernlet = shared_library('kernlet_protocol', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

kernlet_dep = declare_dependency(
	link_with : libkernlet,
	dependencies : deps,
	include_directories : inc
)

install_headers('include/protocols/kernlet/compiler.hpp',
	subdir : 'protocols/kernlet'
)
