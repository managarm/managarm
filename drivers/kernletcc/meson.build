lewis_dep = cxx.find_library('lewis')

kernlet_pb = protoc.process(
	protos/'kernlet/kernlet.proto', 
	extra_args : [ '--proto_path=' + protos/'kernlet']
)

executable('kernletcc', ['src/main.cpp', 'src/fafnir.cpp', kernlet_pb],
	dependencies: [
		coroutines,
		lewis_dep,
		lib_helix_dep,
		proto_lite_dep,
		libmbus_protocol_dep,
		libkernlet_protocol_dep
	],
	install: true
)
