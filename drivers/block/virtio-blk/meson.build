
executable('virtio-block',
	[
		'src/main.cpp',
		'src/block.cpp'
	],
	dependencies: [
		coroutines,
		lib_helix_dep, hw_protocol_dep, libmbus_protocol_dep,
		libblockfs_dep, virtio_core_dep, proto_lite_dep],
	install: true)

