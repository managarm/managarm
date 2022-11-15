inc = [ 'include' ]

i8254x_files = files(
	'src/i8254x.cpp',
	'src/rx.cpp',
)

nic_i8254x_lib = static_library('nic-i8254x', i8254x_files,
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_i8254x_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_i8254x_lib
)
