executable('mbus', 'src/main.cpp',
	dependencies : [core_dep, mbus_proto_dep, frigg, libasync_dep],
	install : true
)

custom_target('mbus-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'mbus.bin',
	input : 'mbus.yml',
	install : true,
	install_dir : server
)
