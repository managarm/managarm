deps = [ libarch, frigg, helix_dep, usb_proto_dep ]
inc = [ 'include' ]

nic_usb_ecm_lib = static_library('nic-usb-ecm', 'src/usb-ecm.cpp',
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_usb_ecm_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_usb_ecm_lib
)
