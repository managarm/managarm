
executable('virtio-block', ['src/main-block.cpp', 'src/block.cpp'],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep, lib_cofiber_dep, hw_protocol_dep, libmbus_protocol_dep,
	 	libblockfs_dep, virtio_core_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	install: true)

