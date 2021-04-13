deps = [
	libarch,
	drm_core_dep, virtio_core_dep, fs_dep,
	hw_dep, mbus_dep, svrctl_dep
]

executable('gfx_virtio', 'src/main.cpp',
	dependencies: deps,
	install: true
)

custom_target('gfx-virtio-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-virtio.bin',
	input: 'gfx-virtio.yml',
	install: true,
	install_dir: server
)
