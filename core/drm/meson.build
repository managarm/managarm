gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

arch_srcs = []

if host_machine.cpu_family() == 'x86_64'
	arch_srcs += ['x86_64-src/copy-sse.S']
elif host_machine.cpu_family() == 'aarch64'
	arch_srcs += ['aarch64-src/copy-fp.S']
else
	error('Unknown architecture')
endif

fs_bragi = gen.process('../../protocols/fs/fs.bragi')
drm_core_inc = include_directories('include/')
drm_core = shared_library('drm_core', [
		'src/core.cpp',
		fs_bragi,
		arch_srcs,
	],
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
