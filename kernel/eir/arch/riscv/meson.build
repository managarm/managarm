# TODO: we need to add eir_generic_sources here.
eir_sources = [
	eir_generic_main,
	files(
		'arch.cpp',
		'cpu.cpp',
		'paging.cpp',
		'stubs.S',
		'../../system/dtb/discovery.cpp'
	),
	eir_generic_sources
]

if build_uefi
	eir_sources += files('platform/virt/early-log.cpp')
endif

eir_includes += include_directories(
	'.',
	'../../system/dtb'
)

if not build_uefi
	subdir('platform/allwinner-d1')
	subdir('platform/virt')
endif
