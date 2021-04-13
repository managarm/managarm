executable('block-ahci',
    [
		'src/main.cpp',
		'src/controller.cpp',
		'src/port.cpp',
		'src/command.cpp'
    ],
	dependencies: [
		coroutines,
		libarch_dep,
		lib_helix_dep,
		hw_protocol_dep,
		libmbus_protocol_dep,
		libblockfs_dep,
		proto_lite_dep],
	install: true)

custom_target('block-ahci-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'block-ahci.bin',
	input: 'block-ahci.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
