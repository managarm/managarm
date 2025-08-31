inc = [ 'include' ]

nic_bcmgenet_lib = static_library('nic-bcmgenet', 'src/main.cpp',
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_bcmgenet_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_bcmgenet_lib
)
