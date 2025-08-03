if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

inc = ['../../../servers/netserver/include', 'include']

nic_freebsd_e1000_import_dep = dependency('e1000-freebsd', method: 'pkg-config')
nic_freebsd_e1000_deps = deps + [ nic_freebsd_e1000_import_dep ]

src_files = files(
	'src/managarm.cpp',
	'src/osdep.cpp',
	'src/irq.cpp',
	'src/rx.cpp',
	'src/tx.cpp',
)

nic_freebsd_e1000_lib = static_library('nic-freebsd-e1000', src_files,
	include_directories : inc,
	dependencies: nic_freebsd_e1000_deps,
	install : true
)

nic_freebsd_e1000_dep = declare_dependency(
	include_directories : inc,
	dependencies : nic_freebsd_e1000_import_dep,
	link_with : nic_freebsd_e1000_lib
)
