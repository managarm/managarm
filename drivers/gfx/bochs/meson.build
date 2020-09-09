gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

fs_bragi = gen.process('../../../protocols/fs/fs.bragi')

executable('gfx_bochs', ['src/main.cpp', fs_bragi],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, drm_core_dep, libfs_protocol_dep,
		hw_protocol_dep, libmbus_protocol_dep, proto_lite_dep],
	install: true)

custom_target('gfx-bochs-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-bochs.bin',
	input: 'gfx-bochs.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
