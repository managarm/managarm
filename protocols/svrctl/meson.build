
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@',
			'@INPUT@'])

svrctl_pb = gen.process('svrctl.proto')
svrctl_proto_includes = include_directories('include/')

libsvrctl_protocol = shared_library(
	'svrctl_protocol',
	[
		'src/server.cpp',
		svrctl_pb
	],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep, lib_cofiber_dep, proto_lite_dep],
	include_directories: svrctl_proto_includes,
	install: true)

libsvrctl_protocol_dep = declare_dependency(
	link_with: libsvrctl_protocol,
	include_directories: svrctl_proto_includes)

install_headers(
	'include/protocols/svrctl/server.hpp',
	subdir: 'protocols/svrctl/')

