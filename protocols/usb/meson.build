usb_bragi = cxxbragi.process('usb.bragi')

inc = [ 'include' ]
src = [ 'src/api.cpp', 'src/hub.cpp', 'src/client.cpp', 'src/server.cpp', usb_bragi ]
deps = [ libarch, helix_dep, frigg ]
headers = [
	'include/protocols/usb/client.hpp',
	'include/protocols/usb/server.hpp',
	'include/protocols/usb/api.hpp',
	'include/protocols/usb/usb.hpp',
	'include/protocols/usb/hub.hpp'
]

libusb = shared_library('usb_protocol', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

usb_proto_dep = declare_dependency(
	link_with : libusb,
	dependencies : deps,
	include_directories : inc
)

install_headers(headers, subdir : 'protocols/usb')
