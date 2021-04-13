eir_generic_sources = files(
	'../common/libc.cpp',
	'../common/font-8x16.cpp',
	'generic/main.cpp',
	'generic/debug.cpp')

eir_includes = [include_directories(
	'../klibc',
	'../common',
	'../../subprojects/lai/include',
	'generic')]

eir_cpp_args = []
eir_c_args = []
eir_link_args = ['-nostdlib']
eir_dependencies = [cxxshim_dep, frigg, libarch_dep]

if get_option('kernel_kasan')
	eir_c_args += [
		'-DEIR_KASAN',
	]
	eir_cpp_args += [
		'-DEIR_KASAN',
	]
endif
if get_option('kernel_log_allocations')
	eir_cpp_args += '-DKERNEL_LOG_ALLOCATIONS'
endif

if arch == 'aarch64'
	subdir('arch/arm/')
elif arch == 'x86_64'
	subdir('arch/x86/')
endif