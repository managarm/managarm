nic_virtio_inc = include_directories('include')
nic_virtio_lib = static_library('nic-virtio', 'src/virtio.cpp',
	include_directories: [
		include_directories('../../../servers/netserver/include'),
		nic_virtio_inc
	],
	dependencies: [
		virtio_core_dep,
		libarch_dep,
		lib_helix_dep,
		clang_coroutine_dep,
	],
	install: true)

nic_virtio_dep = declare_dependency(
	include_directories: nic_virtio_inc,
	dependencies: [
		virtio_core_dep,
	],
	link_with: nic_virtio_lib)
