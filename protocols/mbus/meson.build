mbus_pb = protoc.process('mbus.proto', 
	extra_args : [ '--proto_path=@CURRENT_SOURCE_DIR@']
)

mbus_proto_inc = include_directories('include/')
libmbus_protocol = shared_library('mbus', ['src/client.cpp', mbus_pb],
	dependencies: [
		coroutines,
		lib_helix_dep, proto_lite_dep],
	include_directories: mbus_proto_inc,
	install: true)

libmbus_protocol_dep = declare_dependency(
	link_with: libmbus_protocol,
	include_directories: mbus_proto_inc)

install_headers(
	'include/protocols/mbus/client.hpp',
	subdir: 'protocols/mbus/')
