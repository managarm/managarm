deps = [ virtio_core_dep ]
inc = [ 'include' ]

nic_virtio_lib = static_library('nic-virtio', 'src/virtio.cpp',
	include_directories: [
		'../../../servers/netserver/include', inc
	],
	dependencies: deps,
	install: true
)

nic_virtio_dep = declare_dependency(
	include_directories: inc,
	dependencies: deps,
	link_with: nic_virtio_lib
)
