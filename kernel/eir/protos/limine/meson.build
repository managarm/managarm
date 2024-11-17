if host_machine.cpu_family() not in ['x86_64', 'riscv64']
	subdir_done()
endif

eir_limine_sources = [
	eir_generic_main,
	'entry.cpp',
]

eir_limine_c_args = eir_c_args
eir_limine_cpp_args = eir_cpp_args

if host_machine.cpu_family() == 'x86_64'
	eir_limine_sources += [
		eir64_sources
	]
	eir_limine_c_args += ['-mcmodel=kernel']
	eir_limine_cpp_args += ['-mcmodel=kernel']
elif host_machine.cpu_family() == 'riscv64'
	eir_limine_sources += [
		eir_sources,
		'../../arch/riscv/platform/virt/early-log.cpp',
	]
endif

executable('eir-limine', eir_limine_sources,
	include_directories : [eir_includes],
	dependencies : [freestnd_cxx_hdrs_dep, eir_dependencies],
	c_args: eir_limine_c_args,
	cpp_args: eir_limine_cpp_args,
	link_args: [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/limine.x'],
	link_depends : files('limine.x'),
	install: true
)
