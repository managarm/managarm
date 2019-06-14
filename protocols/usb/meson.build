
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../protocols/usb/', 
			'@INPUT@']) 

libusb_pb = gen.process('../../protocols/usb/usb.proto')
libusb_proto_inc = include_directories('include/')
libusb_protocol = shared_library('usb_protocol', ['src/api.cpp', 'src/client.cpp', 'src/server.cpp', libusb_pb],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, lib_helix_dep, lib_cofiber_dep, proto_lite_dep],
	include_directories: libusb_proto_inc,
	install: true)

libusb_protocol_dep = declare_dependency(
	dependencies: [libarch_dep],
	link_with: libusb_protocol,
	include_directories: libusb_proto_inc)

install_headers(
	'include/protocols/usb/client.hpp',
	'include/protocols/usb/server.hpp',
	'include/protocols/usb/api.hpp',
	'include/protocols/usb/usb.hpp',
	subdir: 'protocols/usb/')

