src = [ 'src/libblockfs.cpp', 'src/gpt.cpp', 'src/ext2fs.cpp' ]
inc = [ 'include' ]
deps = [ fs_proto_dep, mbus_proto_dep, ostrace_proto_dep ]

libblockfs_driver = shared_library('blockfs', src,
	dependencies : deps,
	include_directories : inc,
	install : true
)

libblockfs_dep = declare_dependency(
	link_with : libblockfs_driver,
	dependencies : deps,
	include_directories : inc
)

install_headers('include/blockfs.hpp',
	subdir : 'drivers/libblockfs'
)
