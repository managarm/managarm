eir_sources = [
	eir_generic_main,
	files(
		'load64.S',
		'arch.cpp',
		'fault.S',
		'fault.cpp',
		'generic.cpp',
	),
	eir_generic_sources
]

# TODO: This needs to be reworked to support booting with UEFI on real machines.
if build_uefi
	eir_sources += files('platform/virt/virt.cpp')
else
	eir_sources += files('../../system/dtb/discovery.cpp')
endif

eir_includes += include_directories(
	'.',
	'../../system/dtb'
)

eir_cpp_args += ['-mgeneral-regs-only', '-mno-unaligned-access']
eir_c_args += ['-mgeneral-regs-only', '-mno-unaligned-access']

if not build_uefi
	subdir('platform/raspi4')
	subdir('platform/virt')
endif
