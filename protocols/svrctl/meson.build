svrctl_pb = protoc.process('svrctl.proto', 
	extra_args : [ '--proto_path=@CURRENT_SOURCE_DIR@']
)

svrctl_proto_includes = include_directories('include/')

libsvrctl_protocol = shared_library(
	'svrctl_protocol',
	[
		'src/server.cpp',
		svrctl_pb
	],
	dependencies: [
		coroutines,
		lib_helix_dep, proto_lite_dep],
	include_directories: svrctl_proto_includes,
	install: true)

libsvrctl_protocol_dep = declare_dependency(
	link_with: libsvrctl_protocol,
	include_directories: svrctl_proto_includes)

install_headers(
	'include/protocols/svrctl/server.hpp',
	subdir: 'protocols/svrctl/')

