#ifndef LIBARCH_DMA_POOL_HPP
#define LIBARCH_DMA_POOL_HPP

#if defined(__managarm__)
#	include <arch/os-managarm/dma_pool.hpp>
#else
#	error Unsupported architecture
#endif

namespace arch {
	using os::contiguous_pool;
}

#endif // LIBARCH_DMA_POOL_HPP
