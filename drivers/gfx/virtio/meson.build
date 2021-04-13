executable('gfx_virtio', ['src/main.cpp', fs_bragi],
	dependencies: [
		coroutines,
		libarch_dep,
		lib_helix_dep,
		drm_core_dep,
		virtio_core_dep,
		libfs_protocol_dep,
		hw_protocol_dep,
		libmbus_protocol_dep,
		libsvrctl_protocol_dep,
		proto_lite_dep],
	install: true)

custom_target('gfx-virtio-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-virtio.bin',
	input: 'gfx-virtio.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
