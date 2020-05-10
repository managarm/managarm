executable('virtio-console',
	[
		'src/main.cpp',
		'src/console.cpp'
	],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep,
		hw_protocol_dep,
		libmbus_protocol_dep,
		virtio_core_dep,
		proto_lite_dep
	],
	install: true)

custom_target('tty-virtio-console',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'tty-virtio-console.bin',
	input: 'tty-virtio-console.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
