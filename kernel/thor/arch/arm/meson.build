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
	'trap.cpp',
)

trampoline = custom_target('trampoline',
	command : [
		c.cmd_array(), '-o', '@OUTPUT@',
		'-I' + meson.current_source_dir(),
		'-nostdlib', '-Wl,-Ttext,0', '@INPUT@',
		get_option('cpp_args')
	],
	input : files('trampoline.S'),
	depend_files : files('thor-internal/arch/asm.h'),
	output : 'trampoline.elf'
)

trampoline_binary = custom_target('trampoline-binary',
	command : [
		'aarch64-managarm-objcopy', '-O', 'binary', '-j', '.text', '@INPUT@', '@OUTPUT@'
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
want_acpi = true
