subdir('import')

inc = [ 'include' ]

deps = nic_freebsd_e1000_import_dep

src_files = files(
	'src/managarm.cpp',
	'src/osdep.cpp',
)

nic_freebsd_e1000_lib = static_library('nic-freebsd-e1000', src_files,
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_freebsd_e1000_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_freebsd_e1000_lib
)
