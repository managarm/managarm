
gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@', '--proto_path=@CURRENT_SOURCE_DIR@/../../protocols/clock/', 
			'@INPUT@']) 

clock_pb = gen.process('../../protocols/clock/clock.proto')

executable('clocktracker', ['src/main.cpp', clock_pb],
	dependencies: [
		coroutines,
		lib_helix_dep,
		proto_lite_dep,
		libmbus_protocol_dep,
		clock_protocol_dep
	],
	install: true)

