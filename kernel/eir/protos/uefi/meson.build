eir_uefi_sources = [
	'entry.cpp',
	'helpers.cpp',
]

executable('eir-uefi', eir_uefi_sources + eir64_sources, eir_generic_main,
	include_directories : [eir_includes],
	dependencies : [freestnd_cxx_hdrs_dep, eir_dependencies],
	c_args: eir_c_args,
	cpp_args: eir_cpp_args,
	install : true
)
