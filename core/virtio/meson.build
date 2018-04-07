
virtio_core_inc = include_directories(['include/', '../../frigg/include',
		'../../protocols/hw/include'])
shared_library('libvirtio_core', ['src/core.cpp'],
		cpp_args: ['-DFRIGG_HAVE_LIBC'],
		dependencies: [lib_helix_dep, lib_cofiber_dep, hw_protocol_dep, proto_lite_dep],
		include_directories: virtio_core_inc)

install_headers(
		'include/core/virtio/core.hpp',
		subdir: 'core/virtio/')

