
executable('block-ata', ['src/main.cpp'],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, lib_cofiber_dep, libmbus_protocol_dep,
	 	libblockfs_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	install: true)

