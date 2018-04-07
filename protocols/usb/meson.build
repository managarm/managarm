
gen = generator(protoc,
		output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
		arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../protocols/usb/', 
				'@INPUT@']) 

usb_pb = gen.process('../../protocols/usb/usb.proto')
usb_proto_inc = include_directories('include/')
shared_library('libusb_protocol', ['src/api.cpp', 'src/client.cpp', 'src/server.cpp', usb_pb],
		dependencies: [lib_helix_dep, lib_cofiber_dep, proto_lite_dep],
		include_directories: usb_proto_inc)

install_headers(
		'include/protocols/usb/client.hpp',
		'include/protocols/usb/server.hpp',
		'include/protocols/usb/api.hpp',
		'include/protocols/usb/usb.hpp',
		subdir: 'protocols/usb/')

