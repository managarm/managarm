nic_virtio_inc = [ 'include' ]
nic_virtio_lib = static_library('nic-virtio', 'src/virtio.cpp',
	include_directories: [
		'../../../servers/netserver/include',
		nic_virtio_inc
	],
	dependencies: [
		virtio_core_dep,
		libarch_dep,
		lib_helix_dep,
		coroutines,
	],
	install: true)

nic_virtio_dep = declare_dependency(
	include_directories: nic_virtio_inc,
	dependencies: [
		virtio_core_dep,
	],
	link_with: nic_virtio_lib)
