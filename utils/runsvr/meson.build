svrctl_pb = protoc.process(
	protos/'svrctl/svrctl.proto', 
	extra_args : [ '--proto_path=' + protos/'svrctl']
)

executable('runsvr', ['src/main.cpp', svrctl_pb],
	dependencies: [
		coroutines,
		lib_helix_dep,
		proto_lite_dep,
		libmbus_protocol_dep
	],
	install: true)

