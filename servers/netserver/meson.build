gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@',
			'--proto_path=@CURRENT_SOURCE_DIR@../../protocols/fs',
			'@INPUT@'])

fs_pb = gen.process('../../protocols/fs/fs.proto')

executable('netserver',
	[
		'src/main.cpp',
		'src/nic.cpp',
		'src/ip/ip4.cpp',
		'src/ip/checksum.cpp',
		fs_pb
	],
	dependencies: [
		libarch_dep,
		clang_coroutine_dep,
		lib_helix_dep,
		hw_protocol_dep,
		libfs_protocol_dep,
		libmbus_protocol_dep,
		libsvrctl_protocol_dep,
		proto_lite_dep,
		nic_virtio_dep,
	],
	include_directories: [
		netserver_inc,
		nic_virtio_inc,
	],
	install: true)

custom_target('netserver-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'netserver.bin',
	input: 'netserver.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
