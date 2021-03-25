thor_sources += files(
	'cpu.cpp',
	'user-access.S',
	'paging.cpp',
	'system.cpp',
	'ints.cpp',
	'debug.cpp',
	'entry.S',
	'stubs.S',
	'timer.cpp',
	'gic.cpp',
	'../../system/dtb/dtb.cpp',
	'../../system/pci/pci_dtb.cpp'
)

thor_includes += include_directories('.')

extra_link_args += [
	'-z', 'max-page-size=0x1000',
	'-Wl,-T,' + meson.current_source_dir() + '/link.x'
]

extra_cpp_args += [
	'-DCXXSHIM_INTEGRATE_GCC',
	'-mgeneral-regs-only',
	'-mno-red-zone'
]

extra_c_args += [
	'-mgeneral-regs-only',
	'-mno-red-zone'
]

extra_link_depends += [files('link.x')]
