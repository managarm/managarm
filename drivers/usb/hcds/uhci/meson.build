if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('uhci', 'src/main.cpp',
	dependencies : [ libarch, hw_dep, kernlet_dep, mbus_dep, usb_dep ],
	install : true
)
