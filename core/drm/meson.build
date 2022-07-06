posix_bragi = cxxbragi.process(protos/'posix/posix.bragi')

src = [
	'src/core.cpp',
	posix_bragi
]

if arch == 'x86_64'
	src += 'x86_64-src/copy-sse.S'
elif arch == 'aarch64'
	src += 'aarch64-src/copy-fp.S'
endif

deps = [ libarch, fs_proto_dep, hw_proto_dep, mbus_proto_dep, frigg ]
inc = [ 'include' ]

headers = [
	'include/core/drm/range-allocator.hpp',
	'include/core/drm/id-allocator.hpp',
	'include/core/drm/core.hpp'
]

drm_core = shared_library('drm_core', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

drm_core_dep = declare_dependency(
	link_with : drm_core,
	dependencies : deps,
	include_directories : inc
)

install_headers(headers, subdir : 'core/drm')
