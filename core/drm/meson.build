
gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../bragi/proto', 
				'@INPUT@']) 

fs_pb = gen.process('../../bragi/proto/fs.proto')
drm_core_inc = include_directories(['include/', '../../frigg/include'])
drm_core = shared_library('drm_core', ['src/core.cpp', fs_pb],
		cpp_args: ['-DFRIGG_HAVE_LIBC'],
		dependencies: [lib_helix_dep, lib_cofiber_dep, proto_lite_dep],
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

