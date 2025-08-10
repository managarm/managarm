inc = include_directories('include')
deps = [ libarch_import, hel_dep, frigg ]

if build_kernel
	libarch = declare_dependency(
		dependencies : deps,
		include_directories : inc
	)
elif build_drivers
	libarch_sysdep = shared_library('arch', 'src/dma_pool.cpp', dependencies: deps, include_directories: inc)

	libarch = declare_dependency(
		link_with : libarch_sysdep,
		dependencies : deps,
		include_directories : inc
	)
endif
