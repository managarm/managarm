#include <arch/dma_pool.hpp>
#include <frg/bitops.hpp>
#include <algorithm>
#include <hel-syscalls.h>
#include <hel.h>
#include <helix/ipc.hpp>

namespace arch {

namespace {

constexpr size_t pageSize = 0x1000;

} // namespace

contiguous_pool::contiguous_pool(dma_realm *realm, contiguous_pool_options options)
: realm_{realm}, options_{options} {
	assert(realm_ && "contiguous_pool must be attached to a dma_realm");
	assert(options.addressBits != 0 && "options.addressBits must be provided");
}

dma_ptr contiguous_pool::allocate(size_t size, size_t count, size_t align) {
	auto b = shift_of_(size, count, align);
	auto alloc_size = size_t{1} << b;

	dma_ptr ptr;
	if (b <= max_shift) {
		std::lock_guard lock{bucketMutex_};
		auto bkt = &buckets_[b - min_shift];

		if (bkt->freelist.empty()) {
			auto handle = allocate_pages_(small_region_size);
			auto rn = new dma_memory_region{realm_, this, std::move(handle), 0, small_region_size};
			for (size_t off = 0; off + alloc_size <= small_region_size; off += alloc_size) {
				bkt->freelist.push_back({static_cast<arch::dma_region *>(rn), off});
			}
		}
		assert(!bkt->freelist.empty());

		ptr = bkt->freelist.back();
		bkt->freelist.pop_back();
	} else {
		// Large allocation. Allocate directly from the kernel.
		auto handle = allocate_pages_(alloc_size);
		auto rn = new dma_memory_region{realm_, this, std::move(handle), 0, alloc_size};
		ptr = {static_cast<arch::dma_region *>(rn), 0};
	}

	assert(ptr.get_raw_ptr());
	assert(!(reinterpret_cast<uintptr_t>(ptr.get_raw_ptr()) & (align - 1)));

	return ptr;
}

void contiguous_pool::deallocate(dma_ptr ptr, size_t size, size_t count, size_t align) {
	auto rn = static_cast<dma_memory_region *>(ptr.region());
	assert(!rn->imported());
	assert(ptr.pool() == this);
	auto b = shift_of_(size, count, align);
	auto alloc_size = size_t{1} << b;

	if (b <= max_shift) {
		std::lock_guard lock{bucketMutex_};
		auto bkt = &buckets_[b - min_shift];

		bkt->freelist.push_back(ptr);
	} else {
		// Large allocation. Deallocate directly using the kernel.
		assert(!ptr.offset());
		auto p = reinterpret_cast<void *>(rn->get_base_va());
		deallocate_pages_(p, alloc_size);
		delete rn;
	}
}

dma_space dma_realm::attachDmaSpace(helix::BorrowedDescriptor ioSpace, bool iommuActive) {
	size_t id;
	{
		std::lock_guard lock{spacesMutex_};
		id = attachedDmaSpaces_++;
	}
	assert(id < max_dma_spaces);
	return dma_space{id, this, ioSpace, iommuActive};
}

async::result<void> dma_space::establish_(dma_memory_region *reg) const {
	assert(reg->size);

	// Fast path: the state is immutable once established, hence no lock is required.
	auto state = reg->spaceStates_[index_].load(std::memory_order_acquire);
	if (state && state->established.load(std::memory_order_acquire))
		co_return;

	bool wait = false;
	{
		std::lock_guard guard{realm_->spacesMutex_};

		state = reg->spaceStates_[index_].load(std::memory_order_relaxed);
		if (!state) {
			state = new dma_memory_region::per_space_state{};
			reg->spaceStates_[index_].store(state, std::memory_order_release);
		}

		if (state->established.load(std::memory_order_relaxed))
			co_return;
		if (state->establishing) {
			wait = true;
		} else {
			state->establishing = true;
		}
	}

	// Another coroutine got here first. Wait for it to finish.
	if (wait) {
		co_await state->establishedEvent.wait();
		co_return;
	}

	void *p = nullptr;
	HEL_CHECK(helMapMemory(
		reg->borrowedMemory_.getHandle(),
		space_.getHandle(),
		nullptr,
		reg->backingMemoryOffset_,
		reg->size,
		kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking
				| realm_->options_.dmaMapFlags,
		&p
	));
	auto deviceVa = reinterpret_cast<uintptr_t>(p);
	state->deviceVa = deviceVa;

	// Fault in the whole region asynchronously.
	// With an IOMMU, this installs its PTEs. Without one, it keeps addressToPhysical() from faulting page by page.
	auto populateResult = co_await helix_ng::populateSpace(space_, deviceVa, reg->size);
	HEL_CHECK(populateResult.error());

	if (iommuActive_) {
		// The ioVa is contiguous, so a single range describes the region.
		state->ranges.push_back({0, deviceVa, reg->size});
	} else {
		// The ioVa is fake and only useful for lifetime tracking.
		// The device addresses are physical ones.
		for (size_t offset = 0; offset < reg->size; offset += pageSize) {
			auto chunk = std::min(pageSize, reg->size - offset);
			auto physical = helix::addressToPhysical(space_, deviceVa + offset);

			if (!state->ranges.empty()) {
				auto &back = state->ranges.back();
				if (back.address + back.size == physical) {
					back.size += chunk;
					continue;
				}
			}
			state->ranges.push_back({offset, physical, chunk});
		}
	}

	{
		std::lock_guard guard{realm_->spacesMutex_};
		state->establishing = false;
		state->established.store(true, std::memory_order_release);
	}
	state->establishedEvent.raise();
}

imported_dma_buffer dma_realm::importMemory(helix::BorrowedDescriptor memory, size_t offset, size_t size) {
	auto rn = new dma_memory_region{this, nullptr, std::move(memory), offset, size, true};
	dma_ptr ptr{rn, 0};
	return imported_dma_buffer{this, ptr, size};
}

// Power-of-two that is used for a particular allocation.
int contiguous_pool::shift_of_(size_t size, size_t count, size_t align) {
	return frg::ceil_log2(
		std::max({size * count, align, min_size_class, options_.minAllocationGap})
	);
}

helix::UniqueDescriptor contiguous_pool::allocate_pages_(size_t region_size) {
	HelAllocRestrictions restrictions{};
	restrictions.addressBits = options_.addressBits;

	HelHandle memory;
	HEL_CHECK(helAllocateMemory(region_size, options_.allocateContigous ? kHelAllocContinuous : 0, &restrictions, &memory));

	return helix::UniqueDescriptor{memory};
}

void contiguous_pool::deallocate_pages_(void *p, size_t region_size) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p, region_size));
}

dma_memory_region::~dma_memory_region() {
	{
		std::lock_guard lock{realm_->spacesMutex_};
		// Unmap the region from all DMA spaces first
		for (size_t index = 0; index < max_dma_spaces; ++index) {
			auto state = spaceStates_[index].load(std::memory_order_relaxed);
			if (!state)
				continue;
			if (state->deviceVa) {
				auto dmaSpace = realm_->spaces_[index];
				HEL_CHECK(helUnmapMemory(dmaSpace->descriptor().getHandle(), reinterpret_cast<void *>(*state->deviceVa), size));
			}
			delete state;
		}
	}

	if (base_va) {
		HEL_CHECK(helUnmapMemory(kHelNullHandle, reinterpret_cast<void *>(*base_va), size));
	}
}

} // namespace arch
