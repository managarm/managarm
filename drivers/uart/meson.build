if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('uart', 'src/main.cpp',
	dependencies : [ libarch, fs_proto_dep, hw_proto_dep, mbus_proto_dep, svrctl_proto_dep ],
	install : true
)

custom_target('uart-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'uart.bin',
	input : 'uart.yml',
	install : true,
	install_dir : server
)
