#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/memory-controller.hpp>

namespace thor {

std::expected<frg::unique_ptr<MemoryController, AccountAllocator>, Error> MemoryController::create(
		MemoryController *parent, size_t floor) {
	AccountAllocator alloc{parent};
	frg::unique_ptr<MemoryController, AccountAllocator> ptr{alloc,
			frg::construct<MemoryController>(alloc, CtorToken{})};
	if(!ptr)
		return std::unexpected{Error::noMemory};
	if(parent)
		floor = frg::max(floor, parent->floor_);
	ptr->floor_ = floor;

	// Pools are constructed eagerly, so that all CPUs can access them without synchronization.
	// Each pool is in the shard of its CPU, whose pools free each other's objects through the owner path.
	ptr->pools_ = frg::dyn_array<Pool *, KernelAlloc>{getCpuCount(), *kernelAlloc};
	for(size_t i = 0; i < ptr->pools_.size(); i++) {
		ptr->pools_[i] = frg::construct<Pool>(alloc,
				HeapSlabPolicy{.account = ptr.get(), .shard = static_cast<uint32_t>(i)});
		if(!ptr->pools_[i])
			return std::unexpected{Error::noMemory};
	}
	return ptr;
}

MemoryController::~MemoryController() {
	for(auto pool : pools_) {
		if(!pool)
			continue;
		// Note: this frees through a mismatching allocator.
		frg::destruct(*kernelAlloc, pool);
	}
	assert(!chargedKernel_.load(std::memory_order_relaxed));
}

void *AccountAllocator::refillAndAllocate_(size_t size) const {
	while(true) {
		if(!refillHeapMagazine())
			return nullptr;

		// Retry the allocation after filling the magazine.
		Allocator::Guard guard;
		if(auto p = account_->kernelPoolFor(getCpuData())->allocate(size); p)
			return p;
	}
}

} // namespace thor
