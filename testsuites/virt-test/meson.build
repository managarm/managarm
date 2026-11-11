sources = []

if host_machine.cpu_family() == 'riscv64'
	sources += ['riscv64.cpp']
elif host_machine.cpu_family() == 'x86_64'
	sources += ['x86_64.cpp']
else
	subdir_done()
endif

executable('virt-test',
	sources,
	dependencies: [ hel_dep, core_dep ],
	install : true
)
