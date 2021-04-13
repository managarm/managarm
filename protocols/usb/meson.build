libusb_pb = protoc.process('usb.proto', 
	extra_args : [ '--proto_path=@CURRENT_SOURCE_DIR@']
)

libusb_proto_inc = [ 'include' ]
libusb_protocol = shared_library('usb_protocol', ['src/api.cpp', 'src/client.cpp', 'src/server.cpp', libusb_pb],
	dependencies: [
		coroutines,
		libarch_dep, lib_helix_dep, proto_lite_dep],
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

