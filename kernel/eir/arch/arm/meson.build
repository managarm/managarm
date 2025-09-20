eir_sources = [
	eir_generic_main,
	files(
		'load64.S',
		'arch.cpp',
		'fault.S',
		'fault.cpp',
		'generic.cpp',
		'../../system/dtb/discovery.cpp',
	),
	eir_generic_sources
]

eir_includes += include_directories(
	'.',
	'../../system/dtb'
)

eir_cpp_args += ['-mgeneral-regs-only', '-mno-unaligned-access']
eir_c_args += ['-mgeneral-regs-only', '-mno-unaligned-access']

if not build_uefi
	subdir('platform/raspi4')
endif
