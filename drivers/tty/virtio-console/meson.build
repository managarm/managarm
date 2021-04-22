executable('virtio-console', [ 'src/main.cpp', 'src/console.cpp' ],
	dependencies : [ mbus_proto_dep, virtio_core_dep, kerncfg_proto_dep ],
	install : true
)

custom_target('tty-virtio-console',
	command : [ bakesvr, '-o', '@OUTPUT@', '@INPUT@' ],
	output : 'tty-virtio-console.bin',
	input : 'tty-virtio-console.yml',
	install : true,
	install_dir : server
)
