eir_generic_sources = files(
	'../common/libc.cpp',
	'../common/font-8x16.cpp',
	'generic/main.cpp',
	'generic/memory-layout.cpp',
	'generic/debug.cpp',
	'system/acpi/acpi.cpp',
	'system/acpi/glue.cpp',
)

eir_generic_sources += files('generic/global-constructors.cpp')

eir_generic_main = files(
	'generic/main-function.cpp'
)

eir_includes = [include_directories(
	'../klibc',
	'../common',
	'generic',
	'system/acpi',
)]

cxx = meson.get_compiler('cpp')
libgcc = cxx.find_library('gcc', required: not build_uefi)

eir_c_args = []
eir_cpp_args = ['-fno-threadsafe-statics', '-DFRG_DONT_USE_LONG_DOUBLE']
eir_link_args = ['-nostdlib']
eir_linker_script = files()
eir_dependencies = [libgcc, frigg, libarch]

# Use uACPI in barebones (= table only mode).
eir_c_args += ['-DUACPI_BAREBONES_MODE', '-DUACPI_OVERRIDE_LIBC']
eir_cpp_args += ['-DUACPI_BAREBONES_MODE', '-DUACPI_OVERRIDE_LIBC']

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

# TODO: Using a static_library() would be nicer here but it is not possible
#       since we need to compile it both into 32-bit and 64-bit Multiboot2 code.
eir_generic_sources += uacpi_sources
eir_includes += uacpi_includes

if arch == 'aarch64'
	subdir('arch/arm')
elif arch == 'riscv64'
	subdir('arch/riscv')
elif arch == 'x86_64'
	subdir('arch/x86')
endif

if build_uefi
	subdir('protos/uefi')
else
	subdir('protos/limine')
	subdir('protos/multiboot2')
endif
