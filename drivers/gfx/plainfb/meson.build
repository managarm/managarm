gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../../protocols/fs', 
			'@INPUT@'])
fs_pb = gen.process('../../../protocols/fs/fs.proto')

executable('gfx_plainfb', [
		'src/main.cpp',
		fs_pb
	],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, drm_core_dep,
		libfs_protocol_dep, hw_protocol_dep, libmbus_protocol_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	install: true)

custom_target('gfx-plainfb-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-plainfb.bin',
	input: 'gfx-plainfb.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
