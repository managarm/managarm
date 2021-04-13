executable('gfx_plainfb', 'src/main.cpp',
	dependencies: [ libarch, drm_core_dep, fs_dep, hw_dep, mbus_dep ],
	install: true
)

custom_target('gfx-plainfb-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-plainfb.bin',
	input: 'gfx-plainfb.yml',
	install: true,
	install_dir: server
)
