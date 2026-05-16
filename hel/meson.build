subdir('include')

src = files(
	'src/globals.cpp',
	'src/passthrough-fd.cpp',
)

if build_headers
	install_headers(hel_headers,
		preserve_path: true,
		subdir: 'hel',
	)

	pkgconfig.generate(
		name: 'hel',
		subdirs: ['hel'],
		description: 'hel/helix headers',
	)

	subdir_done()
endif

inc = [ 'include' ]

if not provide_deps and not build_kernel
	deps = [ bragi_dep, frigg, posix_extra_dep, libasync_dep ]

	helix = shared_library('helix', src,
		dependencies : deps,
		include_directories : inc,
		install : true
	)

	helix_dep = declare_dependency(
		link_with : helix,
		dependencies : deps,
		include_directories : inc
	)

	install_headers(hel_headers)
endif

hel_dep = declare_dependency(
	include_directories : inc
)
