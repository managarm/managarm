if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('ps2-hid', 'src/main.cpp',
	dependencies : evbackend_dep,
	install : true
)

custom_target('input-atkbd-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'input-atkbd.bin',
	input : 'input-atkbd.yml',
	install : true,
	install_dir : server
)
