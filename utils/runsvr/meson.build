gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@/../../protocols/svrctl/', 
			'@INPUT@']) 

svrctl_pb = gen.process('../../protocols/svrctl/svrctl.proto')

executable('runsvr', ['src/main.cpp', svrctl_pb],
	dependencies: [
		coroutines,
		lib_helix_dep,
		proto_lite_dep,
		libmbus_protocol_dep
	],
	install: true)

