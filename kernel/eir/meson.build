eir_generic_sources = files(
	'../../frigg/src/debug.cpp',
	'../../frigg/src/libc.cpp',
	'../common/font-8x16.cpp')

eir_includes = include_directories(
	'../include/',
	'../common/',
	'../../frigg/include')

eir_cpp_args = ['-std=c++2a', '-DFRIGG_NO_LIBC', '-Wall']
eir_c_args = ['-std=c11', '-Wall']
eir_link_args = ['-nostdlib']
eir_dependencies = [cxxshim_dep, frigg_dep, libarch_dep]

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

if host_machine.cpu_family() == 'aarch64'
	subdir('arch/arm/')
elif host_machine.cpu_family() == 'x86_64'
	subdir('arch/x86/')
else
	error('Unknown architecture')
endif
