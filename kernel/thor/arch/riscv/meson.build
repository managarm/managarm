src += files(
	'aplic.cpp',
	'cpu.cpp',
	'debug.cpp',
	'entry.S',
	'fp-state.cpp',
	'ints.cpp',
	'paging.cpp',
	'plic.cpp',
	'smp.cpp',
	'stubs.S',
	'system.cpp',
	'timer.cpp',
	'trap.cpp',
	# TODO: Remove unimplemented.cpp after implementing most of the architecture.
	'unimplemented.cpp',
	'user-access.S',
)

# TODO: Remove this after implementing most of the architecture.
cpp_args += ['-Wno-unused-parameter']

inc += include_directories('.')
link_args += [ '-z', 'max-page-size=0x1000' ]

want_dtb = true
want_acpi = true
