
storage_driver_inc = include_directories('../../../../frigg/include/')

executable('storage', ['src/main.cpp'],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep, libmbus_protocol_dep,
		libusb_protocol_dep, libblockfs_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	include_directories: storage_driver_inc,
	install: true)

