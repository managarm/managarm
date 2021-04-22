executable('gfx_plainfb', 'src/main.cpp',
	dependencies : drm_core_dep,
	install : true
)

custom_target('gfx-plainfb-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'gfx-plainfb.bin',
	input : 'gfx-plainfb.yml',
	install : true,
	install_dir : server
)
