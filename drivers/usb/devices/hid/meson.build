executable('hid', 'src/main.cpp',
	dependencies: evbackend_dep,
	install: true
)

custom_target('input-usbhid-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'input-usbhid.bin',
	input: 'input-usbhid.yml',
	install: true,
	install_dir: server
)
