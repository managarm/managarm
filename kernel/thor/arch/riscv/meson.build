src += files(
	'cpu.cpp',
	'debug.cpp',
	'entry.S',
	'fp-state.cpp',
	'ints.cpp',
	'paging.cpp',
	'stubs.S',
	'system.cpp',
	'timer.cpp',
	'trap.cpp',
	# TODO: Remove unimplemented.cpp after implementing most of the architecture.
	'unimplemented.cpp',
	'user-access.S',
)

src += files(
	'../../system/dtb/dtb.cpp',
	'../../system/pci/pci_dtb.cpp'
)

# TODO: Remove this after implementing most of the architecture.
cpp_args += ['-Wno-unused-parameter']

inc += include_directories('.')
link_args += [ '-z', 'max-page-size=0x1000', '-Wl,-T,' + meson.current_source_dir()/'link.x' ]
link_depends += files('link.x')
