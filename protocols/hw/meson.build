
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@', 
			'@INPUT@']) 

hw_pb = gen.process('hw.proto')
hw_proto_inc = include_directories('include/')
libhw_protocol = shared_library('hw_protocol', ['src/client.cpp', hw_pb],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep, proto_lite_dep],
	include_directories: hw_proto_inc,
	install: true)

hw_protocol_dep = declare_dependency(
	link_with: libhw_protocol,
	include_directories: hw_proto_inc)

install_headers(
	'include/protocols/hw/client.hpp',
	subdir: 'protocols/hw/')

