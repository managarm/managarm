executable('clocktracker', 'src/main.cpp',
	dependencies : [ core_dep, mbus_proto_dep, clock_proto_dep ],
	install : true
)

custom_target('clocktracker-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'clocktracker.bin',
	input : 'clocktracker.yml',
	install : true,
	install_dir : server
)
