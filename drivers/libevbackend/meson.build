gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

libevbackend_driver_inc = [ 'include' ]
libevbackend_driver = shared_library('evbackend', ['src/libevbackend.cpp', fs_bragi, hw_pb],
	dependencies: [
		coroutines,
		libarch_dep, libfs_protocol_dep, lib_helix_dep, libmbus_protocol_dep,
		libusb_protocol_dep, proto_lite_dep],
	include_directories: libevbackend_driver_inc,
	install: true)

libevbackend_dep = declare_dependency(
	link_with: libevbackend_driver,
	include_directories: libevbackend_driver_inc)

install_headers(
	'include/libevbackend.hpp',
	subdir: 'drivers/libevbackend/')
