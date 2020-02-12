gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../protocols/mbus', 
			'@INPUT@']) 

mbus_pb = gen.process('../protocols/mbus/mbus.proto')
executable('mbus', ['src/main.cpp', mbus_pb],
	dependencies: [
		clang_coroutine_dep,
		lib_helix_dep, proto_lite_dep],
	install: true)
