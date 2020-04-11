eir_generic_sources = files(
	'../../frigg/src/debug.cpp',
	'../../frigg/src/libc.cpp',
	'../../frigg/src/arch_x86/gdt.cpp',
	'../common/font-8x16.cpp',
	'main.cpp')

eir_includes = include_directories(
	'../include/',
	'../common/',
	'../../frigg/include')

eir32_sources = [files('load32.S', 'i386.cpp'), eir_generic_sources]
eir32_c_args = ['-std=c11', '-m32']
eir32_cpp_args = ['-std=c++17', '-m32', '-DCXXSHIM_INTEGRATE_GCC', '-DFRIGG_NO_LIBC', '-Wall']
eir32_link_args = ['-nostdlib', '-m32', '-Wl,-T,' + meson.current_source_dir() + '/generic32_link.x']
eir32_dependencies = [cxxshim_dep, frigg_dep]
eir32_extra_objects = [meson.current_source_dir() + '/libgcc.a']
eir32_link_depends = files('generic32_link.x')

eir64_sources = [files('load64.S', 'x86_64.cpp'), eir_generic_sources]
eir64_c_args = ['-std=c11', '-m64']
eir64_cpp_args = ['-std=c++17', '-m64', '-DCXXSHIM_INTEGRATE_GCC', '-DFRIGG_NO_LIBC', '-Wall']
eir64_link_args_without_link_script = ['-nostdlib', '-m64', '-Wl,-z,max-page-size=0x1000']
eir64_link_args = ['-Wl,-T,' + meson.current_source_dir() + '/generic64_link.x', eir64_link_args_without_link_script]
eir64_dependencies = [cxxshim_dep, frigg_dep]
eir64_extra_objects = []
eir64_link_depends = files('generic64_link.x')

subdir('multiboot1/')
subdir('multiboot2/')
subdir('stivale/')