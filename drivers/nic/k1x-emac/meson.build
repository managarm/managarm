if host_machine.cpu_family() != 'riscv64'
	subdir_done()
endif

inc = [ 'include' ]

nic_k1x_emac_lib = static_library('nic-k1x-emac', 'src/k1x-emac.cpp',
	include_directories : [ '../../../servers/netserver/include', inc ],
	dependencies : deps,
	install : true
)

nic_k1x_emac_dep = declare_dependency(
	include_directories : inc,
	dependencies : deps,
	link_with : nic_k1x_emac_lib
)
