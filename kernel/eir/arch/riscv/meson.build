# TODO: we need to add eir_generic_sources here.
eir_sources = [
	eir_generic_main,
	files(
		'arch.cpp',
		'cpu.cpp',
		'paging.cpp',
		'stubs.S',
	),
	eir_generic_sources
]

if build_uefi
	eir_sources += files('sbi-platform.cpp')
endif

eir_includes += include_directories(
	'.',
)
