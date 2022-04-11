deps = [ virtio_core_dep ]
inc = [ 'include' ]

nic_rtl8139_lib = static_library('nic-rtl8139', 'src/rtl8139.cpp',
	include_directories : [ '../../../servers/netserver/include', '../../../protocols/fs/include', inc ],
	dependencies : deps,
	install : true
)

nic_rtl8139_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_rtl8139_lib
)
