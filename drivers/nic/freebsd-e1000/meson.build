import = subproject('freebsd-e1000')

inc = ['../../../servers/netserver/include', 'include']
inc += [ import.get_variable('inc') ]

nic_freebsd_e1000_import_lib = static_library('nic-freebsd-e1000-import',
	import.get_variable('src'),
	include_directories : [ inc ],
	install : true,
	c_args : ['-Wno-implicit-fallthrough', '-Wno-unused-parameter']
)

nic_freebsd_e1000_import_dep = declare_dependency(
	include_directories : [ import.get_variable('inc') ],
	link_with : nic_freebsd_e1000_import_lib
)

src_files = files(
	'src/managarm.cpp',
	'src/osdep.cpp',
	'src/irq.cpp',
	'src/rx.cpp',
	'src/tx.cpp',
)

nic_freebsd_e1000_lib = static_library('nic-freebsd-e1000', src_files,
	include_directories : inc,
	dependencies: [ deps, nic_freebsd_e1000_import_dep ],
	install : true
)

nic_freebsd_e1000_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_freebsd_e1000_lib
)
