src += files(
	'../../system/legacy-pc/ata.cpp',
	'early_stubs.S',
	'stubs.S',
	'user-access.S',
	'vmx_stubs.S',
	'cpu.cpp',
	'entry.S',
	'ints.cpp',
	'pic.cpp',
	'system.cpp',
	'paging.cpp',
	'hpet.cpp',
	'rtc.cpp',
	'ept.cpp',
	'pmc-amd.cpp',
	'pmc-intel.cpp',
	'vmx.cpp',
	'debug.cpp',
	'pci_io.cpp'
)

trampoline = custom_target('trampoline',
	command : [ 
		c.cmd_array(), '-o', '@OUTPUT@',
		'-nostdlib', '-Wl,-Ttext,0', '-Wl,--oformat,binary', '@INPUT@', 
		get_option('cpp_args') 
	],
	input : 'trampoline.S',
	output : 'trampoline.bin'
)

embed_trampoline = custom_target('embed-trampoline',
	command : [
		'x86_64-managarm-objcopy', '-I', 'binary', '-O', 'elf64-x86-64',
		'-B', 'i386:x86-64', '@INPUT@', '@OUTPUT@'
	],
	input : trampoline,
	output : 'embed-trampoline.o'
)

src += embed_trampoline
inc += include_directories('.')
link_args += [ '-z', 'max-page-size=0x1000', '-Wl,-T,' + meson.current_source_dir()/'link.x' ]
args += [ '-mcmodel=kernel', '-mno-red-zone', '-mstack-alignment=8' ]
link_depends += files('link.x')

want_acpi = true
