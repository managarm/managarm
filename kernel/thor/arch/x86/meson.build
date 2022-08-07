src += files(
	'../../system/legacy-pc/ata.cpp',
	'early_stubs.S',
	'entry.S',
	'stubs.S',
	'user-access.S',
	'vmx_stubs.S',

	'cpu.cpp',
	'debug.cpp',
	'ept.cpp',
	'hpet.cpp',
	'ints.cpp',
	'pic.cpp',
	'paging.cpp',
	'pmc-amd.cpp',
	'pmc-intel.cpp',
	'pci_io.cpp',
	'rtc.cpp',
	'system.cpp',
	'vmx.cpp'
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
args += [ '-mno-80387', '-mno-mmx', '-mno-sse', '-mno-sse2', '-mcmodel=kernel', '-mno-red-zone', '-mstack-alignment=8' ]
args += ['-DTHOR_ARCH_SUPPORTS_PIO=1']
link_depends += files('link.x')

want_acpi = true
