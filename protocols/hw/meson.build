hw_pb = protoc.process('hw.proto', 
	extra_args : [ '--proto_path=@CURRENT_SOURCE_DIR@']
)

hw_proto_inc = include_directories('include/')
libhw_protocol = shared_library('hw_protocol', ['src/client.cpp', hw_pb],
	dependencies: [
		coroutines,
		lib_helix_dep, proto_lite_dep],
	include_directories: hw_proto_inc,
	install: true)

hw_protocol_dep = declare_dependency(
	link_with: libhw_protocol,
	include_directories: hw_proto_inc)

install_headers(
	'include/protocols/hw/client.hpp',
	subdir: 'protocols/hw/')

