gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

fs_bragi = gen.process('../../protocols/fs/fs.bragi')

executable('uart', ['src/main.cpp', fs_bragi],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, libfs_protocol_dep,
		hw_protocol_dep, libmbus_protocol_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	install: true)

