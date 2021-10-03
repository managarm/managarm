# TODO: we need to add eir_generic_sources here.
eir_sources = [files('entry64.S')]
eir_includes += include_directories('.')

exe = executable('eir-virt', eir_sources,
	include_directories : eir_includes,
	cpp_args : eir_cpp_args,
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	install : true
)
