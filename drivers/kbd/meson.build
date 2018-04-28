
ps2_driver_inc = include_directories('../../frigg/include/')

executable('ps2-hid', ['src/main.cpp'],
	dependencies: [lib_cofiber_dep, libevbackend_dep, lib_helix_dep,
		libmbus_protocol_dep, libusb_protocol_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	include_directories: ps2_driver_inc,
	install: true)

