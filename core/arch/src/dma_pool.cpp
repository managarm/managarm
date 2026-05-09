#include <arch/dma_pool.hpp>
#include <frg/bitops.hpp>
#include <algorithm>
#include <hel-syscalls.h>
#include <hel.h>
#include <ranges>

namespace arch {

namespace {

} // namespace

contiguous_pool::contiguous_pool(contiguous_pool_options options)
: options_{options} {
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
			auto rn = new region{this, std::move(handle), 0, small_region_size};
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
		auto rn = new region{this, std::move(handle), 0, alloc_size};
		ptr = {static_cast<arch::dma_region *>(rn), 0};
	}

	assert(ptr.get_raw_ptr());
	assert(!(reinterpret_cast<uintptr_t>(ptr.get_raw_ptr()) & (align - 1)));

	return ptr;
}

void contiguous_pool::deallocate(dma_ptr ptr, size_t size, size_t count, size_t align) {
	auto rn = static_cast<region *>(ptr.region());
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

dma_space contiguous_pool::attachDmaSpace(helix::BorrowedDescriptor ioSpace, bool iommuActive) {
	size_t id;
	{
		std::lock_guard lock{spacesMutex_};
		id = attachedDmaSpaces_++;
	}
	return dma_space{id, this, ioSpace, iommuActive};
}

imported_dma_buffer contiguous_pool::importMemory(helix::BorrowedDescriptor memory, size_t offset, size_t size) {
	auto rn = new contiguous_pool::region{this, std::move(memory), offset, size, true};
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
	HEL_CHECK(helAllocateMemory(region_size, options_.allocFlags, &restrictions, &memory));

	return helix::UniqueDescriptor{memory};
}

void contiguous_pool::deallocate_pages_(void *p, size_t region_size) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, p, region_size));
}

contiguous_pool::region::~region() {
	auto regionPool = static_cast<contiguous_pool *>(pool());

	if (regionPool) {
		std::lock_guard lock{regionPool->spacesMutex_};
		// Unmap the region from all DMA spaces first
		for (auto [index, ioVa] : dmaSpaces_ | std::views::enumerate) {
			if (ioVa) {
				auto dmaSpace = regionPool->spaces_[index];
				HEL_CHECK(helUnmapMemory(dmaSpace->descriptor().getHandle(), reinterpret_cast<void *>(*ioVa), size));
			}
		}
	}

	if (base_va) {
		HEL_CHECK(helUnmapMemory(kHelNullHandle, reinterpret_cast<void *>(*base_va), size));
	}
}

} // namespace arch
