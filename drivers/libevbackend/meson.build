if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

inc = [ 'include' ]
deps = [ libarch, fs_dep, usb_dep, mbus_dep, hw_dep ]

libevbackend_driver = shared_library('evbackend', 'src/libevbackend.cpp',
	dependencies : deps,
	include_directories : inc,
	install : true
)

evbackend_dep = declare_dependency(
	link_with : libevbackend_driver,
	dependencies : deps,
	include_directories : inc
)

install_headers('include/libevbackend.hpp',
	subdir : 'drivers/libevbackend'
)
