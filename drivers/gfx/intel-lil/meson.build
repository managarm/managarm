if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

src = files(
	'src/gfx.cpp',
	'src/main.cpp',
	'src/imports.cpp',
)

executable('gfx_intel_lil', src,
	dependencies : [ drm_core_dep, lil_dep, lil_header_dep, svrctl_proto_dep ],
	install : true
)

custom_target('gfx-intel-lil-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'gfx-intel-lil.bin',
	input : 'gfx-intel-lil.yml',
	install : true,
	install_dir : server
)
