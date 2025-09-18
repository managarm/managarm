#pragma once

#include <arch/dma_structs.hpp>
#include <frg/slab.hpp>
#include <mutex>
#include <stddef.h>
#include <stdint.h>

namespace arch {

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

	frg::slab_pool<
		contiguous_policy,
		std::mutex
	> _slab;
};

} // namespace arch
