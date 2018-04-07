
libarch_inc = include_directories('include/')
shared_library('libarch_dep', 'src/dma_pool.cpp',
		include_directories: libarch_inc)

install_headers(
		'include/arch/bits.hpp',
		'include/arch/dma_pool.hpp',
		'include/arch/dma_structs.hpp',
		'include/arch/io_space.hpp',
		'include/arch/mem_space.hpp',
		'include/arch/register.hpp',
		'include/arch/variable.hpp',
		subdir: 'arch/')
install_headers(
		'include/arch/os-managarm/dma_pool.hpp',
		subdir: 'arch/os-managarm/')
install_headers(
		'include/arch/x86/mem_space.hpp',
		'include/arch/x86/io_space.hpp',
		subdir: 'arch/x86/')

