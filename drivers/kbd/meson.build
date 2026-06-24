if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

kbd_sources = files(
	'src/main.cpp',
	'src/mouse/elantech.cpp',
)

executable('ps2-hid', kbd_sources,
	dependencies : [ evbackend_dep, hw_proto_dep ],
	install : true
)

custom_target('input-atkbd-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'input-atkbd.bin',
	input : 'input-atkbd.yml',
	install : true,
	install_dir : server
)
