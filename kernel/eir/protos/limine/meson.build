if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

eir_limine_sources = [
	'entry.cpp'
]

executable('eir-limine', eir_limine_sources + eir64_sources, eir_generic_main,
	include_directories : [eir_includes],
	dependencies : [freestnd_cxx_hdrs_dep, eir_dependencies],
	c_args: [eir_c_args, '-mcmodel=kernel'],
	cpp_args: [eir_cpp_args, '-mcmodel=kernel'],
	link_args: [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/limine.x'],
	install: true
)
