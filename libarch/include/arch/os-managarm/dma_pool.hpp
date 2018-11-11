#ifndef LIBARCH_OS_MANAGARM_DMA_POOL_HPP
#define LIBARCH_OS_MANAGARM_DMA_POOL_HPP

#include <stddef.h>
#include <stdint.h>
#include <mutex>
#include <utility>
#include <arch/dma_structs.hpp>
#include <frg/slab.hpp>

namespace arch {
namespace os {

struct contiguous_policy {
public:
	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);
};

struct contiguous_pool : dma_pool {
	contiguous_pool();

	void *allocate(size_t size, size_t count, size_t align) override;
	void deallocate(void *pointer, size_t size, size_t count, size_t align) override;

private:
	contiguous_policy _policy;

	frg::slab_allocator<
		contiguous_policy,
		std::mutex
	> _allocator;
};

} } // namespace arch::os

#endif // LIBARCH_OS_MANAGARM_DMA_POOL_HPP
