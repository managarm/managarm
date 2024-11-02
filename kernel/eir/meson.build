eir_generic_sources = files(
	'../common/libc.cpp',
	'../common/font-8x16.cpp',
	'generic/main.cpp',
	'generic/debug.cpp')

eir_generic_main = files(
	'generic/main-function.cpp'
)

eir_includes = [include_directories(
	'../klibc',
	'../common',
	'generic')]

eir_includes += uacpi_includes

cxx = meson.get_compiler('cpp')
libgcc = cxx.find_library('gcc', required: not build_uefi)

eir_c_args = []
eir_cpp_args = ['-fno-threadsafe-statics', '-DFRG_DONT_USE_LONG_DOUBLE']
eir_link_args = ['-nostdlib']
eir_dependencies = [libgcc, frigg, libarch]

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
	subdir('arch/arm')
elif arch == 'riscv64'
	subdir('arch/riscv')
elif arch == 'x86_64'
	subdir('arch/x86')
endif

if build_uefi
	eir_cpp_args = []
	subdir('protos/uefi')
endif
