protobuf_dep = dependency('protobuf')
protoc_dep = dependency('protoc')

pb2frigg = executable('pb2frigg', 'src/main.cpp',
	dependencies : [ protobuf_dep, protoc_dep ],
	install : true
)
