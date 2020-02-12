
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../protocols/fs', 
			'@INPUT@'])
fs_pb = gen.process('../../protocols/fs/fs.proto')

libblockfs_driver_inc = include_directories('include/')
libblockfs_driver = shared_library('blockfs', ['src/libblockfs.cpp', 'src/gpt.cpp',
		'src/ext2fs.cpp', fs_pb],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep, libfs_protocol_dep, libmbus_protocol_dep,
		proto_lite_dep],
	include_directories: libblockfs_driver_inc,
	install: true)

libblockfs_dep = declare_dependency(
	link_with: libblockfs_driver,
	include_directories: libblockfs_driver_inc)

install_headers(
	'include/blockfs.hpp',
	subdir: 'drivers/libblockfs/')

