src += files(
	'entry.S',
	'stubs.S',
	'user-access.S',

	'cpu.cpp',
	'debug.cpp',
	'gic.cpp',
	'ints.cpp',
	'paging.cpp',
	'system.cpp',
	'timer.cpp',
	'../../system/dtb/dtb.cpp',
	'../../system/pci/pci_dtb.cpp'
)

inc += include_directories('.')

link_args += [ '-z', 'max-page-size=0x1000', '-Wl,-T,' + meson.current_source_dir()/'link.x' ]
args += [ '-mgeneral-regs-only', '-mno-red-zone' ]
link_depends += files('link.x')
