sources = []

if host_machine.cpu_family() == 'riscv64'
	sources += ['riscv64.cpp']
else
	sources += ['x86_64.cpp']
endif

executable('virt-test',
	sources,
	dependencies: [ hel_dep ],
	install : true
)
