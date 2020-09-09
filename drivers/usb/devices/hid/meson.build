
executable('hid', ['src/main.cpp'],
	dependencies: [
		clang_coroutine_dep,
		libevbackend_dep, lib_helix_dep,
		libfs_protocol_dep, libmbus_protocol_dep, libusb_protocol_dep, proto_lite_dep],
	install: true)

custom_target('input-usbhid-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'input-usbhid.bin',
	input: 'input-usbhid.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
