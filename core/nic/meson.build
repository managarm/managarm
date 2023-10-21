inc = [ 'include' ]
deps = [ libarch, hw_proto_dep, net_proto_dep, mbus_proto_dep, svrctl_proto_dep ]

headers = [
	'include/core/nic/core.hpp',
	'include/core/nic/buffer.hpp'
]

src = [
	'src/core.cpp',
	'src/buffer.cpp'
]

nic_core = shared_library('nic_core', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

nic_core_dep = declare_dependency(
	link_with : nic_core,
	dependencies : deps,
	include_directories : inc
)

install_headers(headers,
	subdir : 'core/nic'
)
