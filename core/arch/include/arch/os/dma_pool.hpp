#pragma once

#include <arch/dma_structs.hpp>
#include <array>
#include <helix/ipc.hpp>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace arch {

struct contiguous_pool_options {
	size_t addressBits{32};
	// Gap in bytes between the base addresses of two allocations.
	// When doing non-coherent DMA, this is needed to ensure that different allocations
	// end up on different cache lines (such that writes to an allocation cannot cause
	// adjacent cache lines to become dirty).
	size_t minAllocationGap{64};
};

struct dma_space;

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
	friend dma_space;

	contiguous_pool(contiguous_pool_options options = {});

	dma_ptr allocate(size_t size, size_t count, size_t align) override;
	void deallocate(dma_ptr ptr, size_t size, size_t count, size_t align) override;

	dma_space attachDmaSpace(helix::BorrowedDescriptor ioSpace);

private:
	struct region : dma_region {
		friend dma_space;

		region(
		    contiguous_pool *pool,
		    helix::UniqueDescriptor backingMemory,
		    void *p,
		    size_t s
		)
		: dma_region{pool, s},
		  backingMemory_{std::move(backingMemory)} {
			base_va = reinterpret_cast<uintptr_t>(p);
		}

	private:
		helix::UniqueDescriptor backingMemory_;

		std::vector<std::optional<uintptr_t>> dmaSpaces_;
	};

	struct bucket {
		std::vector<dma_ptr> freelist;
	};

	int shift_of_(size_t size, size_t count, size_t align);

	std::pair<void *, helix::UniqueDescriptor> allocate_pages_(size_t region_size);
	void deallocate_pages_(void *p, size_t region_size);

	contiguous_pool_options options_;

	size_t attachedDmaSpaces_ = 0;

	std::mutex mutex_;

	// Protected by mutex_.
	std::array<bucket, num_size_classes> buckets_;
};

struct dma_space {
	dma_space(size_t i, contiguous_pool *p, helix::BorrowedDescriptor space)
	: index_{i}, pool_{p}, space_{space} {}

	template <typename T>
	requires requires (T t) {
		{ t.dmaPtr() } -> std::same_as<dma_ptr>;
	}
	uintptr_t iova_of(T &view) {
		dma_ptr dp = view.dmaPtr();
		auto region = static_cast<contiguous_pool::region *>(dp.region());

		if (region->dmaSpaces_.size() <= index_)
			region->dmaSpaces_.resize(index_ + 1);

		if (!region->dmaSpaces_[index_]) {
			assert(region->size);

			void *p = nullptr;
			HEL_CHECK(helMapMemory(
				region->backingMemory_.getHandle(),
				space_.getHandle(),
				nullptr,
				0,
				*region->size,
				kHelMapProtRead | kHelMapProtWrite | kHelMapPopulate,
				&p
			));
			// TODO: touchRange instead of kHelMapPopulate

			region->dmaSpaces_[index_] = reinterpret_cast<uintptr_t>(p);
		}

		return region->dmaSpaces_[index_].value() + dp.offset();
	}

private:
	size_t index_;
	[[maybe_unused]] contiguous_pool *pool_;
	helix::BorrowedDescriptor space_;
};

} // namespace arch
