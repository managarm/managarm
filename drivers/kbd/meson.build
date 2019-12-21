
ps2_driver_inc = include_directories('../../frigg/include/')

executable('ps2-hid', ['src/main.cpp'],
	dependencies: [
		clang_coroutine_dep,
		libarch_dep, libevbackend_dep, lib_helix_dep,
		libfs_protocol_dep, libmbus_protocol_dep, libusb_protocol_dep, proto_lite_dep],
	cpp_args: ['-DFRIGG_HAVE_LIBC'],
	include_directories: ps2_driver_inc,
	install: true)

custom_target('input-atkbd-server',
	command: [bakesvr, '-o', '@OUTPUT@', '@INPUT@'],
	output: 'input-atkbd.bin',
	input: 'input-atkbd.yml',
	install: true,
	install_dir: get_option('libdir') / 'managarm/server')
