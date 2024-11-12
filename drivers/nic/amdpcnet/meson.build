inc = [ 'include' ]

nic_pcnet_lib = static_library('nic-pcnet', 'src/pcnet.cpp',
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_pcnet_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_pcnet_lib
)
