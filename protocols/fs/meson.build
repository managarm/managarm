fs_bragi_files = files('fs.bragi')

headers = [ 'include/protocols/fs/client.hpp', 'include/protocols/fs/common.hpp' ]

if build_headers
	install_headers(headers, subdir : 'protocols/fs')
	install_data(fs_bragi_files, install_dir: get_option('datadir') / 'managarm/protocols/fs')

	pkgconfig.generate(
		name: 'managarm_protocols_fs',
		subdirs: ['.'],
		description: 'Managarm\'s fs protocol',
		variables: ['bragi=${pc_sysrootdir}/${prefix}/${datadir}/managarm/protocols/fs/fs.bragi'],
	)
	protocol_pc_files += 'managarm_protocols_fs'

	subdir_done()
endif

fs_bragi = cxxbragi.process('fs.bragi')

inc = [ 'include' ]
src = [ 'src/client.cpp', 'src/server.cpp', 'src/file-locks.cpp', fs_bragi ]

if not provide_deps
	deps = [ helix_dep, frigg, ostrace_proto_dep, core_dep, libasync_dep, libsmarter ]

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
