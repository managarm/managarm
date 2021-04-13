executable('virtio-console', [ 'src/main.cpp', 'src/console.cpp' ],
	dependencies: [ hw_dep, mbus_dep, virtio_core_dep, kerncfg_dep ],
	install: true
)

custom_target('tty-virtio-console',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'tty-virtio-console.bin',
	input: 'tty-virtio-console.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server'
)
