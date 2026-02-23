#pragma once

#include <arch/dma_structs.hpp>
#include <array>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace arch {

struct contiguous_pool_options {
	size_t addressBits{32};
};

struct contiguous_pool : dma_pool {
private:
	// log2 of the min/max size classes.
	static constexpr int min_shift = 3;
	static constexpr int max_shift = 14;

	static constexpr size_t min_size_class = size_t{1} << min_shift;
	static constexpr size_t max_size_class = size_t{1} << max_shift;
	static constexpr size_t num_size_classes = max_shift - min_shift + 1;

	// Size of regions that store objects of size <= max_size_class.
	static constexpr int small_region_size = size_t{1} << 16;

public:
	contiguous_pool(contiguous_pool_options options = {});

	dma_ptr allocate(size_t size, size_t count, size_t align) override;
	void deallocate(dma_ptr ptr, size_t size, size_t count, size_t align) override;

private:
	struct region : dma_region {
		region(contiguous_pool *pool, void *p)
		: dma_region{pool} {
			base_va = reinterpret_cast<uintptr_t>(p);
		}
	};

	struct bucket {
		std::vector<dma_ptr> freelist;
	};

	int shift_of_(size_t size, size_t count, size_t align);

	void *allocate_pages_(size_t region_size);
	void deallocate_pages_(void *p, size_t region_size);

	contiguous_pool_options options_;

	std::mutex mutex_;

	// Protected by mutex_.
	std::array<bucket, num_size_classes> buckets_;
};

} // namespace arch
