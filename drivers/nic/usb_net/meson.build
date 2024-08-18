deps = [ libarch, frigg, helix_dep, usb_proto_dep, mbus_proto_dep, fs_proto_dep ]
inc = [ 'include' ]

nic_usb_net_src = files(
	'src/usb-ecm.cpp',
	'src/usb-ncm.cpp',
	'src/usb-net.cpp',
	'src/usb-mbim.cpp',
)

nic_usb_net_lib = static_library('nic-usb-net', nic_usb_net_src,
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_usb_net_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_usb_net_lib
)
