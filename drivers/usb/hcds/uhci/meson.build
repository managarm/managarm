if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('uhci', 'src/main.cpp',
	dependencies : [ libarch, core_dep, hw_proto_dep, kernlet_proto_dep, mbus_proto_dep, usb_proto_dep ],
	install : true
)

custom_target('uhci-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'uhci.bin',
	input : 'uhci.yml',
	install : true,
	install_dir : server
)
