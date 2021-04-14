executable('gfx_vmware', 'src/main.cpp',
	dependencies : [ drm_core_dep, svrctl_dep ],
	install : true
)

custom_target('gfx-vmware-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'gfx-vmware.bin',
	input : 'gfx-vmware.yml',
	install : true,
	install_dir : server
)
