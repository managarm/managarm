src += files(
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

inc += include_directories('.')

link_args += [ '-z', 'max-page-size=0x1000', '-Wl,-T,' + meson.current_source_dir()/'link.x' ]
args += [ '-mgeneral-regs-only', '-mno-red-zone' ]
link_depends += files('link.x')
