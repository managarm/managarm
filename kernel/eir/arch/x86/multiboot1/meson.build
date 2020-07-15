mb1_sources = files('multiboot1.S', 'multiboot1.cpp')
executable('eir-mb1', eir32_sources, mb1_sources,
	include_directories: eir_includes,
	c_args: eir32_c_args,
	cpp_args: eir32_cpp_args,
	link_args: eir32_link_args,
	dependencies: eir32_dependencies,
	objects: eir32_extra_objects,
	link_depends: eir32_link_depends,
	install: true)