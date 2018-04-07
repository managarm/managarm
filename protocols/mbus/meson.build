
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../../bragi/proto/', 
			'@INPUT@']) 

mbus_pb = gen.process('../../bragi/proto/mbus.proto')
mbus_proto_inc = include_directories('include/')
libmbus_protocol = shared_library('mbus', ['src/client.cpp', mbus_pb],
	dependencies: [lib_helix_dep, lib_cofiber_dep, proto_lite_dep],
	include_directories: mbus_proto_inc,
	install: true)

libmbus_protocol_dep = declare_dependency(
	link_with: libmbus_protocol,
	include_directories: mbus_proto_inc)

install_headers(
	'include/protocols/mbus/client.hpp',
	subdir: 'protocols/mbus/')

