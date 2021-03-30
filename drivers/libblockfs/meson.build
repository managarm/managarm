gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

bragi_gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

fs_bragi = gen.process('../../protocols/fs/fs.bragi')

libblockfs_driver_inc = include_directories('include/')
libblockfs_driver = shared_library('blockfs',
	[
		'src/libblockfs.cpp',
		'src/gpt.cpp',
		'src/ext2fs.cpp',
		fs_bragi,
		bragi_gen.process('../../protocols/ostrace/ostrace.bragi')
	],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep,
		libfs_protocol_dep,
		libmbus_protocol_dep,
		libostrace_protocol_dep,
		proto_lite_dep],
	include_directories: libblockfs_driver_inc,
	install: true)

libblockfs_dep = declare_dependency(
	link_with: libblockfs_driver,
	include_directories: libblockfs_driver_inc)

install_headers(
	'include/blockfs.hpp',
	subdir: 'drivers/libblockfs/')
