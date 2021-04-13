
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@',
			'@INPUT@'])

kernlet_pb = gen.process('kernlet.proto')
kernlet_proto_inc = include_directories('include/')
libkernlet_protocol = shared_library('kernlet_protocol', ['src/compiler.cpp', kernlet_pb],
	dependencies: [
		coroutines,
		lib_helix_dep, proto_lite_dep, libmbus_protocol_dep],
	include_directories: kernlet_proto_inc,
	install: true)

libkernlet_protocol_dep = declare_dependency(
	link_with: libkernlet_protocol,
	include_directories: kernlet_proto_inc)

install_headers(
	'include/protocols/kernlet/compiler.hpp',
	subdir: 'protocols/kernlet/')

