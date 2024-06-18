kernlet_bragi = cxxbragi.process('kernlet.bragi')

src = [ 'src/compiler.cpp', kernlet_bragi ]
inc = [ 'include' ]

if build_drivers
	deps = [ mbus_proto_dep, frigg ]

	libkernlet = shared_library('kernlet_protocol', src,
		dependencies : deps,
		include_directories : inc,
		install : true
	)

	kernlet_proto_dep = declare_dependency(
		link_with : libkernlet,
		dependencies : deps,
		sources : kernlet_bragi,
		include_directories : inc
	)

	install_headers('include/protocols/kernlet/compiler.hpp',
		subdir : 'protocols/kernlet'
	)
endif
