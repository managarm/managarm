gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@',
			'--proto_path=@CURRENT_SOURCE_DIR@../../../protocols/fs',
			'@INPUT@'])

fs_pb = gen.process('../../../protocols/fs/fs.proto')

executable('nic-virtio',
	[
		'src/main.cpp',
		'src/net.cpp',
		fs_pb
	],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep,
		hw_protocol_dep,
		libfs_protocol_dep,
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
