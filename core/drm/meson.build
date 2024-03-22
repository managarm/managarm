posix_bragi = cxxbragi.process(protos/'posix/posix.bragi')

src = [
	'src/core.cpp',
	'src/device.cpp',
	'src/fourcc.cpp',
	'src/ioctl.cpp',
	'src/mode-object.cpp',
	'src/property.cpp',
	posix_bragi
]

if arch == 'x86_64'
	src += 'x86_64-src/copy-sse.S'
elif arch == 'aarch64'
	src += 'aarch64-src/copy-fp.S'
elif arch == 'riscv64'
	src += 'riscv64-src/copy-fp.S'
endif

deps = [ libarch, fs_proto_dep, hw_proto_dep, mbus_proto_dep, frigg, core_dep ]
inc = [ 'include' ]

headers = [
	'include/core/drm/range-allocator.hpp',
	'include/core/drm/core.hpp',
	'include/core/drm/debug.hpp',
	'include/core/drm/device.hpp',
	'include/core/drm/fwd-decls.hpp',
	'include/core/drm/mode-object.hpp',
	'include/core/drm/property.hpp'
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
