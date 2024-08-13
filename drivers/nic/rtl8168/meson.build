deps += [ core_dep, hw_proto_dep ]
inc = [ 'include' ]

rtl8168_files = files(
	'src/rtl8168.cpp',
	'src/rx.cpp',
	'src/tx.cpp',
	'src/waits.cpp',
	'src/phy.cpp',
	'src/hw-config.cpp',
	'src/regs.cpp'
)

nic_rtl8168_lib = static_library('nic-rtl8168', rtl8168_files,
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_rtl8168_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_rtl8168_lib
)
