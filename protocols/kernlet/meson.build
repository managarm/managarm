kernlet_pb = protoc.process('kernlet.proto', 
	extra_args : [ '--proto_path=@CURRENT_SOURCE_DIR@']
)

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

