src += files(
	'cpu.cpp',
	'debug.cpp',
	'entry.S',
	'ints.cpp',
	'paging.cpp',
	'stubs.S',
	# TODO: Remove the stubs.cpp file after moving its functions elsewhere.
	'stubs.cpp',
	'system.cpp',
	# TODO: Remove unimplemented.cpp after implementing most of the architecture.
	'unimplemented.cpp',
)

# TODO: Remove this after implementing most of the architecture.
cpp_args += ['-Wno-unused-parameter']

inc += include_directories('.')
link_args += [ '-z', 'max-page-size=0x1000', '-Wl,-T,' + meson.current_source_dir()/'link.x' ]
link_depends += files('link.x')
