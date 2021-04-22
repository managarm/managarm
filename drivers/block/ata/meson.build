if host_machine.cpu_family() != 'x86_64'
	subdir_done()
endif

executable('block-ata', 'src/main.cpp',
	dependencies : [ libarch, hw_proto_dep, mbus_proto_dep, libblockfs_dep ],
	install : true
)
