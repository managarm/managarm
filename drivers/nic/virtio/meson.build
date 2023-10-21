inc = [ 'include' ]

executable('nic-virtio', 'src/virtio.cpp',
	include_directories : [ inc ],
	dependencies : [ net_proto_dep, svrctl_proto_dep, virtio_core_dep, nic_core_dep, core_dep ],
	install : true
)

custom_target('nic-virtio-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'nic-virtio.bin',
	input : 'nic-virtio.yml',
	install : true,
	install_dir : server
)
