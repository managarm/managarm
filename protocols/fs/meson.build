fs_bragi = cxxbragi.process('fs.bragi')

inc = [ 'include' ]
src = [ 'src/client.cpp', 'src/server.cpp', 'src/file-locks.cpp', fs_bragi ]
headers = [ 'include/protocols/fs/client.hpp', 'include/protocols/fs/common.hpp' ]

fs_bragi_files = files('fs.bragi')

if not provide_deps
	deps = [ helix_dep, frigg ]

	libfs = shared_library('fs_protocol', src,
		dependencies : deps,
		include_directories : inc,
		install : true
	)

	fs_proto_dep = declare_dependency(
		link_with : libfs,
		dependencies : deps,
		sources : fs_bragi,
		include_directories : inc
	)

	install_headers(headers, subdir : 'protocols/fs')
endif
