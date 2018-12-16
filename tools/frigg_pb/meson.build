
lib_protobuf_dep = compiler.find_library('protobuf')
lib_protoc_dep = compiler.find_library('protoc')

executable('frigg_pb', 'src/main.cpp',
	dependencies: [lib_protobuf_dep, lib_protoc_dep],
	install: true)

