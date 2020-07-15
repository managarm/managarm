stivale_sources = files('stivale.S', 'stivale.cpp')
executable('eir-stivale', eir64_sources, stivale_sources,
	include_directories: eir_includes,
	c_args: eir64_c_args,
	cpp_args: eir64_cpp_args,
	link_args: [eir64_link_args_without_link_script, '-Wl,-T,' + meson.current_source_dir() + '/stivale_link.x'],
	dependencies: eir64_dependencies,
	link_depends: files('stivale_link.x'),
	install: true)
