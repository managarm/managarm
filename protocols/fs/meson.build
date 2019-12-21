
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../bragi/proto', 
			'@INPUT@']) 

fs_pb = gen.process('../../bragi/proto/fs.proto')
fs_proto_inc = include_directories('include/')
libfs_protocol = shared_library('fs_protocol', ['src/client.cpp', 'src/server.cpp', 'src/file-locks.cpp', fs_pb],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep, proto_lite_dep],
	include_directories: fs_proto_inc,
	install: true)

libfs_protocol_dep = declare_dependency(
	link_with: libfs_protocol,
	include_directories: fs_proto_inc)

install_headers(
	'include/protocols/fs/client.hpp',
	'include/protocols/fs/common.hpp',
	subdir: 'protocols/fs/')

