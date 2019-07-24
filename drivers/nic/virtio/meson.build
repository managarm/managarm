
executable('nic-virtio',
	[
		'src/main.cpp',
		'src/net.cpp'
	],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep,
		hw_protocol_dep,
		libmbus_protocol_dep,
		libsvrctl_protocol_dep,
		virtio_core_dep,
		proto_lite_dep
	],
	install: true)

custom_target('nic-virtio-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'nic-virtio.bin',
	input: 'nic-virtio.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
