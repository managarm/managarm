src += files(
	'entry.S',
	'stubs.S',
	'stubs.cpp',
	'paging.cpp',
	'cpu.cpp',
	'system.cpp',
	'debug.cpp'
	)

inc += include_directories('.')
link_args += [ '-z', 'max-page-size=0x1000', '-Wl,-T,' + meson.current_source_dir()/'link.x' ]
link_depends += files('link.x')
