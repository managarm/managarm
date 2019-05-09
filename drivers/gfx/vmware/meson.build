
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../../bragi/proto', 
			'@INPUT@'])
fs_pb = gen.process('../../../bragi/proto/fs.proto')

executable('gfx_vmware', ['src/main.cpp', fs_pb],
	dependencies: [libarch_dep, lib_helix_dep, lib_cofiber_dep, drm_core_dep, libfs_protocol_dep,
		hw_protocol_dep, libmbus_protocol_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	install: true)

