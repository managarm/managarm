executable('gfx_virtio', 'src/main.cpp',
	dependencies : [ drm_core_dep, virtio_core_dep,  svrctl_dep ],
	install : true
)

custom_target('gfx-virtio-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'gfx-virtio.bin',
	input : 'gfx-virtio.yml',
	install : true,
	install_dir : server
)
