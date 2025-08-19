src += files(
	'entry.S',
	'stubs.S',
	'user-access.S',

	'cpu.cpp',
	'debug.cpp',
	'gic.cpp',
	'gic_v2.cpp',
	'gic_v3.cpp',
	'ints.cpp',
	'paging.cpp',
	'system.cpp',
	'smp.cpp',
	'timer.cpp',
)

trampoline = custom_target('trampoline',
	command : [
		c.cmd_array(), '-o', '@OUTPUT@',
		'-nostdlib', '-Wl,-Ttext,0', '@INPUT@',
		get_option('cpp_args')
	],
	input : 'trampoline.S',
	output : 'trampoline.elf'
)

trampoline_binary = custom_target('trampoline-binary',
	command : [
		'aarch64-managarm-objcopy', '@INPUT@', '--dump-section', '.text=@OUTPUT@'
	],
	input : trampoline,
	output : 'trampoline.bin'
)

embed_trampoline = custom_target('embed-trampoline',
	command : [
		'aarch64-managarm-objcopy', '-I', 'binary', '-O', 'aarch64-managarm-elf',
		'-B', 'aarch64', '@INPUT@', '@OUTPUT@'
	],
	input : trampoline_binary,
	output : 'embed-trampoline.o')

src += embed_trampoline
inc += include_directories('.')

link_args += [ '-z', 'max-page-size=0x1000' ]

args += [ '-mgeneral-regs-only', '-mno-red-zone' ]

want_dtb = true
