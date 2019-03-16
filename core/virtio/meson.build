
virtio_core_inc = include_directories(['include/', '../../frigg/include',
		'../../protocols/hw/include'])
virtio_core = shared_library('virtio_core', ['src/core.cpp'],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	dependencies: [libarch_dep, lib_helix_dep, lib_cofiber_dep,
		hw_protocol_dep, libkernlet_protocol_dep, proto_lite_dep],
	include_directories: virtio_core_inc,
	install: true)

virtio_core_dep = declare_dependency(
	dependencies: [libarch_dep],
	link_with: virtio_core,
	include_directories: virtio_core_inc)

install_headers(
		'include/core/virtio/core.hpp',
		subdir: 'core/virtio/')

