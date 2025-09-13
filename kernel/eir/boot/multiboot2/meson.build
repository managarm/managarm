if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

mb2_sources = files('multiboot2.S', 'multiboot2.cpp')

eir_includes += include_directories('.')

executable('eir-mb2', eir32_sources, mb2_sources, eir_generic_main,
	include_directories : eir_includes,
	c_args : eir32_c_args,
	cpp_args : eir32_cpp_args,
	link_args : [eir32_link_args, '-Wl,-T' + eir32_linker_script.get(0).full_path()],
	dependencies : eir32_dependencies,
	objects : eir32_extra_objects,
	link_depends : [eir32_link_depends, eir32_linker_script],
	install : true
)
