eir_efi_sources = [
	'paging-efi.cpp',
	eir_sources
]

eir_efi_cpp_args = [
	eir_cpp_args,
	'-DEIR_EFI'
]

exe = executable('eir-efi', eir_efi_sources, 'limine.cpp',
	include_directories : eir_includes,
	cpp_args : eir_efi_cpp_args,
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	install : true
)
