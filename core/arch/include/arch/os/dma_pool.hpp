#pragma once

#include <arch/dma_structs.hpp>
#include <array>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace arch {

struct contiguous_pool_options {
	size_t addressBits{};
	// Gap in bytes between the base addresses of two allocations.
	// When doing non-coherent DMA, this is needed to ensure that different allocations
	// end up on different cache lines (such that writes to an allocation cannot cause
	// adjacent cache lines to become dirty).
	size_t minAllocationGap{64};
	// TODO: default to non-continous, so that devices requiring contigous physical allocations
	// have to opt-in to this behavior.
	int allocFlags = kHelAllocContinuous;
	// Additional flags passed when mapping the memory into DMA space.
	uint32_t dmaMapFlags = 0;
};

struct dma_space;
struct imported_dma_buffer;

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

	dma_space attachDmaSpace(helix::BorrowedDescriptor ioSpace, bool iommuActive);

	imported_dma_buffer importMemory(helix::BorrowedDescriptor memory, size_t offset, size_t size);

	struct region : dma_region {
		friend dma_space;

		region(
		    contiguous_pool *pool,
		    helix::UniqueDescriptor backingMemory,
		    size_t backingMemoryOffset,
		    size_t s
		)
		: dma_region{pool},
		  size{s},
		  backingMemory_{std::move(backingMemory)},
		  borrowedMemory_{backingMemory_},
		  backingMemoryOffset_{backingMemoryOffset},
		  imported_{false} {
			void *p = nullptr;
			HEL_CHECK(helMapMemory(borrowedMemory_.getHandle(), kHelNullHandle, nullptr, backingMemoryOffset, s,
			kHelMapProtRead | kHelMapProtWrite, &p));
			base_va = reinterpret_cast<uintptr_t>(p);
		}

		region(
		    contiguous_pool *pool,
		    helix::BorrowedDescriptor borrowedMemory,
		    size_t backingMemoryOffset,
		    size_t s,
		    bool imported = false
		)
		: dma_region{pool},
		  size{s},
		  backingMemory_{},
		  borrowedMemory_{std::move(borrowedMemory)},
		  backingMemoryOffset_{backingMemoryOffset},
		  imported_{imported} {
			void *p = nullptr;
			HEL_CHECK(helMapMemory(borrowedMemory_.getHandle(), kHelNullHandle, nullptr, backingMemoryOffset, s,
			kHelMapProtRead | kHelMapProtWrite, &p));
			base_va = reinterpret_cast<uintptr_t>(p);
		}

		~region();

		bool imported() const {
			return imported_;
		}

		std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() const {
			return {borrowedMemory_, backingMemoryOffset_};
		}

	private:
		size_t size;

		helix::UniqueDescriptor backingMemory_;
		helix::BorrowedDescriptor borrowedMemory_;
		size_t backingMemoryOffset_;

		// Holds the `ioVa` of the region in the DMA space, if already mapped there.
		std::vector<std::optional<uintptr_t>> dmaSpaces_;
		bool imported_;
	};

private:
	struct bucket {
		std::vector<dma_ptr> freelist;
	};

	int shift_of_(size_t size, size_t count, size_t align);

	helix::UniqueDescriptor allocate_pages_(size_t region_size);
	void deallocate_pages_(void *p, size_t region_size);

	contiguous_pool_options options_;

	size_t attachedDmaSpaces_ = 0;
	std::mutex spacesMutex_;
	std::vector<dma_space *> spaces_;

	std::mutex bucketMutex_;
	// Protected by mutex_.
	std::array<bucket, num_size_classes> buckets_;
};

struct imported_dma_buffer {
	imported_dma_buffer() : pool_{nullptr}, size_{0} {}

	imported_dma_buffer(contiguous_pool *pool, dma_ptr ptr, size_t size)
	: pool_{pool}, ptr_{ptr}, size_{size} {}

	~imported_dma_buffer() {
		auto rn = static_cast<contiguous_pool::region *>(ptr_.region());
		delete rn;
	}

	friend void swap(imported_dma_buffer &a, imported_dma_buffer &b) {
		using std::swap;
		swap(a.pool_, b.pool_);
		swap(a.ptr_, b.ptr_);
		swap(a.size_, b.size_);
	}

	imported_dma_buffer(const imported_dma_buffer &) = delete;
	imported_dma_buffer &operator=(const imported_dma_buffer &) = delete;

	imported_dma_buffer(imported_dma_buffer &&other) noexcept
	: arch::imported_dma_buffer() {
		swap(*this, other);
	}

	imported_dma_buffer &operator=(imported_dma_buffer other) noexcept {
		swap(*this, other);
		return *this;
	}

	operator dma_buffer_view() const {
		return dma_buffer_view{ptr_, size_};
	}

	dma_buffer_view view() const {
		return dma_buffer_view{ptr_, size_};
	}

private:
	contiguous_pool *pool_;
	dma_ptr ptr_;
	size_t size_;
};

struct dma_space {
	dma_space(size_t i, contiguous_pool *p, helix::BorrowedDescriptor space, bool iommuActive)
	: index_{i}, pool_{p}, space_{space}, iommuActive_{iommuActive} {
		std::lock_guard lock{pool_->spacesMutex_};
		pool_->spaces_.insert(pool_->spaces_.begin() + i, this);
	}

	dma_space(const dma_space &) = delete;
	dma_space &operator=(const dma_space &) = delete;
	dma_space(dma_space &&) = delete;
	dma_space &operator=(dma_space &&) = delete;

	~dma_space() {
		// TODO(no92): we should detach the space from its pool and unmap all remaining memory
	}

	template <dma_view T>
	async::result<uintptr_t> iova_of(T &&view) const {
		dma_ptr dp = view.get_dma_ptr();
		auto reg = static_cast<contiguous_pool::region *>(dp.region());
		uintptr_t iova_base;

		{
			std::lock_guard guard{pool_->spacesMutex_};

			if (reg->dmaSpaces_.size() <= index_)
				reg->dmaSpaces_.resize(index_ + 1);

			if (!reg->dmaSpaces_[index_]) {
				assert(reg->size);

				void *p = nullptr;
				HEL_CHECK(helMapMemory(
					reg->borrowedMemory_.getHandle(),
					space_.getHandle(),
					nullptr,
					reg->backingMemoryOffset_,
					reg->size,
					kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking | pool_->options_.dmaMapFlags,
					&p
				));

				reg->dmaSpaces_[index_] = reinterpret_cast<uintptr_t>(p);
			}

			iova_base = reg->dmaSpaces_[index_].value();
		}

		// Ensure that the backing pages of the view are already present, or fauled in.
		co_await helix_ng::populateSpace(
		    space_, iova_base + dp.offset(), view.size()
		);

		// If no IOMMU is active, the ioVa address is fake and only useful for lifetime tracking.
		// We need to return a physical address instead.
		if (!iommuActive_)
			co_return helix::addressToPhysical(space_, iova_base + dp.offset());

		// If we have an IOMMU, we can simply return the ioVa.
		co_return iova_base + dp.offset();
	}

	bool iommuActive() const {
		return iommuActive_;
	}

	helix::BorrowedDescriptor descriptor() const {
		return space_;
	}

private:
	size_t index_;
	contiguous_pool *pool_;
	helix::BorrowedDescriptor space_;
	bool iommuActive_;
};

} // namespace arch
