
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../../bragi/proto', 
			'@INPUT@'])
fs_pb = gen.process('../../../bragi/proto/fs.proto')

executable('gfx_virtio', ['src/main.cpp', fs_pb],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep,
		lib_helix_dep,
		lib_cofiber_dep,
		drm_core_dep,
		virtio_core_dep,
		libfs_protocol_dep,
		hw_protocol_dep,
		libmbus_protocol_dep,
		libsvrctl_protocol_dep,
		proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	install: true)

custom_target('gfx-virtio-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-virtio.bin',
	input: 'gfx-virtio.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')

