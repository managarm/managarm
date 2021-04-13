deps = [
	libarch, 
	drm_core_dep, fs_dep, hw_dep, 
	mbus_dep, svrctl_dep
]

executable('gfx_vmware', 'src/main.cpp',
	dependencies: deps,
	install: true
)

custom_target('gfx-vmware-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-vmware.bin',
	input: 'gfx-vmware.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server'
)
