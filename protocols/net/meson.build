net_bragi = cxxbragi.process('net.bragi')

net_proto_dep = declare_dependency(
	sources : net_bragi,
	include_directories : 'include'
)

install_headers('include/protocols/net/defs.hpp',
	subdir : 'protocols/net'
)
