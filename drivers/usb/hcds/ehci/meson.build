executable('ehci', 'src/main.cpp',
	dependencies : [ libarch, hw_proto_dep, mbus_proto_dep, usb_proto_dep, kernlet_proto_dep ],
	install : true
)

custom_target('ehci-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'ehci.bin',
	input : 'ehci.yml',
	install : true,
	install_dir : server
)
