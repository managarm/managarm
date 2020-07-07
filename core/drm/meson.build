gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

fs_bragi = gen.process('../../protocols/fs/fs.bragi')
drm_core_inc = include_directories(['include/', '../../frigg/include'])
drm_core = shared_library('drm_core', [
		'src/core.cpp',
		fs_bragi,
		'x86_64-src/copy-sse.S',
	],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, libfs_protocol_dep,
		hw_protocol_dep, libmbus_protocol_dep, proto_lite_dep],
	include_directories: drm_core_inc,
	install: true)

drm_core_dep = declare_dependency(
	link_with: drm_core,
	include_directories: drm_core_inc)

install_headers(
		'include/core/drm/range-allocator.hpp',
		'include/core/drm/id-allocator.hpp',
		'include/core/drm/core.hpp',
		subdir: 'core/drm/')
