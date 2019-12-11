lib_protobuf_dep = cpp_compiler.find_library('protobuf')
lib_protoc_dep = cpp_compiler.find_library('protoc')

executable('pb2frigg', 'src/main.cpp',
	dependencies: [lib_protobuf_dep, lib_protoc_dep],
	install: true)
