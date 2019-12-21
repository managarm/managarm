
lewis_dep = cpp_compiler.find_library('lewis')

gen = generator(protoc,
	output: ['@BASENAME@.pb.h', '@BASENAME@.pb.cc'],
	arguments: ['--cpp_out=@BUILD_DIR@',
			'--proto_path=@CURRENT_SOURCE_DIR@/../../protocols/kernlet/',
			'@INPUT@'])

kernlet_pb = gen.process('../../protocols/kernlet/kernlet.proto')

executable('kernletcc', ['src/main.cpp', 'src/fafnir.cpp', kernlet_pb],
	dependencies: [
		clang_coroutine_dep,
		lewis_dep,
		lib_helix_dep,
		proto_lite_dep,
		libmbus_protocol_dep,
		libkernlet_protocol_dep
	],
	install: true)

