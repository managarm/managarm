executable('virtio-block', [ 'src/main.cpp', 'src/block.cpp' ],
	dependencies : [ libblockfs_dep, virtio_core_dep ],
	install : true
)

custom_target('virtio-block-server',
	command : [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output : 'virtio-block.bin',
	input : 'virtio-block.yml',
	install : true,
	install_dir : server
)
