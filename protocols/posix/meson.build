posix_bragi_files = files('posix.bragi')

headers = [ 'include/protocols/posix/data.hpp', 'include/protocols/posix/supercalls.hpp' ]

if build_headers
	install_headers(headers, subdir : 'protocols/posix')
	install_data(posix_bragi_files, install_dir: get_option('datadir') / 'managarm/protocols/posix')

	pkgconfig.generate(
		name: 'managarm_protocols_posix',
		subdirs: ['.'],
		description: 'Managarm\'s posix protocol',
		variables: ['bragi=${pc_sysrootdir}/${prefix}/${datadir}/managarm/protocols/posix/posix.bragi'],
	)
	protocol_pc_files += 'managarm_protocols_posix'

	subdir_done()
endif

inc = [ 'include' ]

posix_extra_dep = declare_dependency(
	include_directories : inc
)

if not provide_deps
	install_headers(headers, subdir : 'protocols/posix')
endif
