eir_sources = [
	eir_generic_main,
	files(
		'load64.S',
		'arch.cpp',
		'fault.S',
		'fault.cpp',
		'platform/raspi4/raspi4.cpp',
	),
	eir_generic_sources
]

eir_includes += include_directories(
	'.',
)

eir_cpp_args += ['-mgeneral-regs-only', '-mno-unaligned-access']
eir_c_args += ['-mgeneral-regs-only', '-mno-unaligned-access']
