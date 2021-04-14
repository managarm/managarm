executable('gfx_bochs', 'src/main.cpp',
	dependencies : drm_core_dep,
	install : true
)

custom_target('gfx-bochs-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'gfx-bochs.bin',
	input : 'gfx-bochs.yml',
	install : true,
	install_dir : server
)
