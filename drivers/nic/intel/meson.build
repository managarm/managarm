deps = [ libarch, hw_proto_dep ]
inc = [ 'include' ]

nic_intel_lib = static_library('nic-intel', 'src/e1000.cpp',
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_intel_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_intel_lib
)
