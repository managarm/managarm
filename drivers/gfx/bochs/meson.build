executable('gfx_bochs', 'src/main.cpp',
	dependencies: [ libarch, drm_core_dep, fs_dep, hw_dep, mbus_dep ],
	install: true
)

custom_target('gfx-bochs-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'gfx-bochs.bin',
	input: 'gfx-bochs.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server'
)
