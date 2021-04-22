src = [ 
	'src/main.cpp',
	'src/controller.cpp',
	'src/port.cpp',
	'src/command.cpp'
]

executable('block-ahci', src,
	dependencies : [ libarch, hw_proto_dep, mbus_proto_dep, libblockfs_dep ],
	install : true
)

custom_target('block-ahci-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'block-ahci.bin',
	input : 'block-ahci.yml',
	install : true,
	install_dir : server
)
