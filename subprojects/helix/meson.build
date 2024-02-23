project('helix', 'cpp')

coroutines = dependency('cxxshim-coroutine-std',
				 required: false,
				 fallback: ['cxxshim', 'std_coroutine_dep'])

bragi = subproject('bragi').get_variable('bragi_dep')

frigg = dependency('frigg')

hel = subproject('hel').get_variable('hel_dep')

deps = [ hel, coroutines, bragi, frigg ]

helix = library('helix', 'src/globals.cpp',
	dependencies : deps,
	install : true
)

helix_dep = declare_dependency(
	link_with : helix,
	dependencies : deps
)

pkg = import('pkgconfig')
pkg.generate(
	name : 'helix',
	description : 'Hel support library'
)

meson.override_dependency('helix', helix_dep)
