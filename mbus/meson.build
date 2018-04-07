
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@../bragi/proto', 
			'@INPUT@']) 

mbus_pb = gen.process('../bragi/proto/mbus.proto')
executable('mbus', ['src/main.cpp', mbus_pb],
	dependencies: [lib_helix_dep, lib_cofiber_dep, proto_lite_dep],
	install: true)

