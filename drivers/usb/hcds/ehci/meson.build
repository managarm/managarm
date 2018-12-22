
ehci_driver_inc = include_directories('../../../../frigg/include/')

executable('ehci', ['src/main.cpp'],
	dependencies: [libarch_dep, lib_helix_dep, lib_cofiber_dep, hw_protocol_dep, 
		libmbus_protocol_dep, libusb_protocol_dep, libkernlet_protocol_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	include_directories: ehci_driver_inc,
	install: true)

