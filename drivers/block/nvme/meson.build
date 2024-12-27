src = [
	'src/main.cpp',
	'src/controller.cpp',
	'src/queue.cpp',
	'src/command.cpp',
	'src/namespace.cpp',
]

executable('block-nvme', src,
	dependencies : [ libarch, hw_proto_dep, mbus_proto_dep, libblockfs_dep, svrctl_proto_dep ],
	install : true
)

custom_target('block-nvme-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'block-nvme.bin',
	input : 'block-nvme.yml',
	install : true,
	install_dir : server
)
