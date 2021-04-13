executable('netserver',
	[
		'src/main.cpp',
		'src/nic.cpp',
		'src/ip/ip4.cpp',
		'src/ip/checksum.cpp',
		'src/ip/arp.cpp',
		'src/ip/udp4.cpp',
		'src/ip/tcp4.cpp',
		fs_bragi
	],
	dependencies: [
		libarch_dep,
		coroutines,
		lib_helix_dep,
		hw_protocol_dep,
		libfs_protocol_dep,
		libmbus_protocol_dep,
		libsvrctl_protocol_dep,
		proto_lite_dep,
		nic_virtio_dep,
	],
	include_directories: [ 'include', nic_virtio_inc ],
	install: true)

custom_target('netserver-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'netserver.bin',
	input: 'netserver.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
