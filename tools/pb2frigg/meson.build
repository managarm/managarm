lib_protobuf_dep = dependency('protobuf')
lib_protoc_dep = dependency('protoc')

executable('pb2frigg', 'src/main.cpp',
	dependencies: [lib_protobuf_dep, lib_protoc_dep],
	install: true)
