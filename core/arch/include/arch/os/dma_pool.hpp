#pragma once

#include <algorithm>
#include <arch/dma_structs.hpp>
#include <array>
#include <async/oneshot-event.hpp>
#include <async/result.hpp>
#include <atomic>
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
	bool allocateContigous = true;
};

struct dma_realm_options {
	// Additional flags passed when mapping memory into the DMA spaces of this realm.
	uint32_t dmaMapFlags = 0;
};

struct dma_space;
struct dma_realm;
struct imported_dma_buffer;

// Upper bound on the number of DMA spaces that can be attached to a single realm.
inline constexpr size_t max_dma_spaces = 8;

// Piece of a region that is contiguous in the device address space of a dma_space.
struct dma_range {
	// Offset into the region.
	size_t offset;
	// Device address that `offset` maps to.
	uintptr_t address;
	size_t size;
};

struct dma_memory_region : dma_region {
	friend dma_space;
	friend dma_realm;

	dma_memory_region(
	    dma_realm *realm,
	    dma_pool *pool,
	    helix::UniqueDescriptor backingMemory,
	    size_t backingMemoryOffset,
	    size_t s
	)
	: dma_region{pool},
	  realm_{realm},
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

	dma_memory_region(
	    dma_realm *realm,
	    dma_pool *pool,
	    helix::BorrowedDescriptor borrowedMemory,
	    size_t backingMemoryOffset,
	    size_t s,
	    bool imported = false
	)
	: dma_region{pool},
	  realm_{realm},
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

	~dma_memory_region();

	bool imported() const {
		return imported_;
	}

	std::pair<helix::BorrowedDescriptor, uint64_t> getMemory() const {
		return {borrowedMemory_, backingMemoryOffset_};
	}

private:
	// State of this region in one DMA space of its realm.
	struct per_space_state {
		// Address of the region in the DMA space.
		// Immutable after established is true.
		std::optional<uintptr_t> deviceVa;
		// Device addresses of the whole region, in ascending offset order.
		// Immutable after established is true.
		std::vector<dma_range> ranges;
		// Set once mapping, populating and translating is done.
		std::atomic<bool> established{false};
		// Set while a coroutine establishes this state.
		// Protected by dma_realm::spacesMutex_.
		bool establishing = false;
		// Raised when established becomes true.
		async::oneshot_event establishedEvent;
	};

	dma_realm *realm_;
	size_t size;

	helix::UniqueDescriptor backingMemory_;
	helix::BorrowedDescriptor borrowedMemory_;
	size_t backingMemoryOffset_;

	std::array<std::atomic<per_space_state *>, max_dma_spaces> spaceStates_{};
	bool imported_;
};

// Set of DMA spaces that a driver uses. Pools attach to a realm.
// The realm is responsible for mapping regions into DMA spaces and tearing them down again.
struct dma_realm {
	friend dma_space;
	friend dma_memory_region;

	dma_realm(dma_realm_options options = {})
	: options_{options} {}

	dma_realm(const dma_realm &) = delete;
	dma_realm &operator=(const dma_realm &) = delete;

	dma_space attachDmaSpace(helix::BorrowedDescriptor ioSpace, bool iommuActive);

	// Wraps foreign memory into a region of this realm.
	imported_dma_buffer importMemory(helix::BorrowedDescriptor memory, size_t offset, size_t size);

private:
	dma_realm_options options_;

	size_t attachedDmaSpaces_ = 0;
	std::mutex spacesMutex_;
	std::vector<dma_space *> spaces_;
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
	contiguous_pool(dma_realm *realm, contiguous_pool_options options = {});

	dma_ptr allocate(size_t size, size_t count, size_t align) override;
	void deallocate(dma_ptr ptr, size_t size, size_t count, size_t align) override;

	dma_realm *realm() const {
		return realm_;
	}

private:
	struct bucket {
		std::vector<dma_ptr> freelist;
	};

	int shift_of_(size_t size, size_t count, size_t align);

	helix::UniqueDescriptor allocate_pages_(size_t region_size);
	void deallocate_pages_(void *p, size_t region_size);

	dma_realm *realm_;
	contiguous_pool_options options_;

	std::mutex bucketMutex_;
	// Protected by mutex_.
	std::array<bucket, num_size_classes> buckets_;
};

struct imported_dma_buffer {
	imported_dma_buffer() : realm_{nullptr}, ptr_{}, size_{0} {}

	imported_dma_buffer(dma_realm *realm, dma_ptr ptr, size_t size)
	: realm_{realm}, ptr_{ptr}, size_{size} {
		assert(realm_);
	}

	~imported_dma_buffer() {
		if (realm_) {
			auto rn = static_cast<dma_memory_region *>(ptr_.region());
			delete rn;
		}
	}

	friend void swap(imported_dma_buffer &a, imported_dma_buffer &b) {
		using std::swap;
		swap(a.realm_, b.realm_);
		swap(a.ptr_, b.ptr_);
		swap(a.size_, b.size_);
	}

	imported_dma_buffer(const imported_dma_buffer &) = delete;

	imported_dma_buffer(imported_dma_buffer &&other) noexcept
	: imported_dma_buffer() {
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
	dma_realm *realm_;
	dma_ptr ptr_;
	size_t size_;
};

struct dma_space {
	dma_space(size_t i, dma_realm *realm, helix::BorrowedDescriptor space, bool iommuActive)
	: index_{i}, realm_{realm}, space_{space}, iommuActive_{iommuActive} {
		std::lock_guard lock{realm_->spacesMutex_};
		realm_->spaces_.insert(realm_->spaces_.begin() + i, this);
	}

	dma_space(const dma_space &) = delete;
	dma_space &operator=(const dma_space &) = delete;
	dma_space(dma_space &&) = delete;
	dma_space &operator=(dma_space &&) = delete;

	~dma_space() {
		// TODO(no92): we should detach the space from its realm and unmap all remaining memory
	}

	// True if the region backing the view is already established in this space.
	// Lets hot paths skip the ensure_mapped() coroutine entirely.
	template <dma_view T>
	bool check_mapped(T &&view) const {
		auto st = state_of_(view.get_dma_ptr());
		return st && st->established.load(std::memory_order_acquire);
	}

	// Establishes the whole region backing the view in this space.
	template <dma_view T>
	async::result<void> ensure_mapped(T &&view) const {
		// Obtain the region before forwarding to the coroutine
		// such that we do not access dangling pointers when view is a reference.
		auto region = static_cast<dma_memory_region *>(view.get_dma_ptr().region());
		return establish_(region);
	}

	// Device address of the view's first byte.
	// The view has to be contiguous in device address space.
	// Requires a prior ensure_mapped() of the view.
	template <dma_view T>
	uintptr_t iova_of(T &&view) const {
		dma_ptr dp = view.get_dma_ptr();
		auto st = state_of_(dp);
		assert(st && st->established.load(std::memory_order_acquire));
		auto rg = range_of_(st, dp.offset());
		assert(dp.offset() + view.size_bytes() <= rg->offset + rg->size);
		return rg->address + (dp.offset() - rg->offset);
	}

	bool iommuActive() const {
		return iommuActive_;
	}

	helix::BorrowedDescriptor descriptor() const {
		return space_;
	}

private:
	dma_memory_region::per_space_state *state_of_(dma_ptr dp) const {
		auto reg = static_cast<dma_memory_region *>(dp.region());
		return reg->spaceStates_[index_].load(std::memory_order_acquire);
	}

	// Range that covers `offset`. The ranges are sorted and cover the whole region.
	static const dma_range *range_of_(dma_memory_region::per_space_state *st, size_t offset) {
		auto it = std::upper_bound(
		    st->ranges.begin(),
		    st->ranges.end(),
		    offset,
		    [](size_t o, const dma_range &r) { return o < r.offset; }
		);
		assert(it != st->ranges.begin());
		return &*(it - 1);
	}

	async::result<void> establish_(dma_memory_region *reg) const;

	size_t index_;
	dma_realm *realm_;
	helix::BorrowedDescriptor space_;
	bool iommuActive_;
};

} // namespace arch
