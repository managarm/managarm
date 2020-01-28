eir_sources = files(
	'../../frigg/src/debug.cpp',
	'../../frigg/src/libc.cpp',
	'../../frigg/src/arch_x86/gdt.cpp',
	'../common/font-8x16.cpp',
	'main.cpp',
	'load.S')

eir_includes = include_directories(
		'../include/',
		'../common/',
		'../../frigg/include')

eir_c_args = ['-std=c11', '-m32']
eir_cpp_args = ['-std=c++17', '-m32', '-DCXXSHIM_INTEGRATE_GCC', '-DFRIGG_NO_LIBC', '-Wall']
eir_link_args = ['-m32', '-nostdlib', '-Wl,-T,' + meson.current_source_dir() + '/link.x']
eir_dependencies = [cxxshim_dep, frigg_dep]
eir_extra_objects = ['./libgcc.a']
eir_link_depends = files('link.x')

mb1_sources = files('multiboot1/multiboot1.S', 'multiboot1/multiboot1.cpp')
executable('eir-mb1', eir_sources, mb1_sources,
	include_directories: eir_includes,
	c_args: eir_c_args,
	cpp_args: eir_cpp_args,
	link_args: eir_link_args,
	dependencies: eir_dependencies,
	objects: eir_extra_objects,
	link_depends: eir_link_depends,
	install: true)


mb2_sources = files('multiboot2/multiboot2.S', 'multiboot2/multiboot2.cpp')
executable('eir-mb2', eir_sources, mb2_sources,
	include_directories: eir_includes,
	c_args: eir_c_args,
	cpp_args: eir_cpp_args,
	link_args: eir_link_args,
	dependencies: eir_dependencies,
	objects: eir_extra_objects,
	link_depends: eir_link_depends,
	install: true)
