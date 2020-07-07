gen = generator(bragi, arguments: [
		'-l', 'stdc++',
		'--protobuf',
		'@INPUT@',
		'@OUTPUT@'
	],
	output: '@BASENAME@.bragi.hpp')

fs_bragi = gen.process('../../protocols/fs/fs.bragi')

gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../protocols/hw/', 
			'@INPUT@']) 
hw_pb = gen.process('../../protocols/hw/hw.proto')

libevbackend_driver_inc = include_directories('include/')
libevbackend_driver = shared_library('evbackend', ['src/libevbackend.cpp', fs_bragi, hw_pb],
	dependencies: [
		clang_coroutine_dep,
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
