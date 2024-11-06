src = files(
	'src/main.cpp',
	'src/quirks.cpp',
	'src/handler/multitouch.cpp',
	'src/quirks/wacom.cpp',
)

executable('hid', src,
	dependencies : [evbackend_dep, core_dep],
	install : true
)

custom_target('input-usbhid-server',
	command : [ bakesvr, '-o', '@OUTPUT@', '@INPUT@' ],
	output : 'input-usbhid.bin',
	input : 'input-usbhid.yml',
	install : true,
	install_dir : server
)
