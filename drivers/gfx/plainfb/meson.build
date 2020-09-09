gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')
fs_bragi = gen.process('../../../protocols/fs/fs.bragi')

executable('gfx_plainfb', [
		'src/main.cpp',
		fs_bragi
	],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, drm_core_dep,
		libfs_protocol_dep, hw_protocol_dep, libmbus_protocol_dep, proto_lite_dep],
	install: true)

custom_target('gfx-plainfb-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-plainfb.bin',
	input: 'gfx-plainfb.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
