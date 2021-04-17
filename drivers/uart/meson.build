if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('uart', 'src/main.cpp',
	dependencies : [ libarch, fs_dep, hw_dep, mbus_dep ],
	install : true
)
