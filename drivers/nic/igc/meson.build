inc = [ 'include' ]

nic_igc_lib = static_library('nic-igc', 'src/main.cpp',
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_igc_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_igc_lib
)
