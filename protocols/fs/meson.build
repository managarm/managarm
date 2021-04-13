gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

fs_bragi = gen.process('fs.bragi')
fs_proto_inc = include_directories('include/')
libfs_protocol = shared_library('fs_protocol', ['src/client.cpp', 'src/server.cpp', 'src/file-locks.cpp', fs_bragi],
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

