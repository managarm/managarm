svrctl_bragi = cxxbragi.process('svrctl.bragi')

src = [ 'src/server.cpp', svrctl_bragi ]
inc = [ 'include' ]

if not provide_deps
	deps = [ core_dep, helix_dep, libsmarter ]

	libsvrctl = shared_library('svrctl_protocol', src,
		dependencies : deps,
		include_directories : inc,
		install : true
	)

	svrctl_proto_dep = declare_dependency(
		link_with : libsvrctl,
		dependencies : deps,
		sources : svrctl_bragi,
		include_directories : inc
	)

	install_headers('include/protocols/svrctl/server.hpp',
		subdir : 'protocols/svrctl'
	)
endif
