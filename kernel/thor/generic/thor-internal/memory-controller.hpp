#pragma once

#include <atomic>
#include <expected>
#include <frg/dyn_array.hpp>
#include <frg/unique.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-heap.hpp>

namespace thor {

struct MemoryController {
private:
	struct CtorToken {};

public:
	using Pool = frg::sharded_slab_pool<HeapSlabPolicy>;

	// parent is null for the root controller.
	// The floor (in pages) is raised to the floor of the parent.
	static std::expected<frg::unique_ptr<MemoryController, KernelAlloc>, Error> create(
			MemoryController *parent, size_t floor);

	MemoryController(CtorToken) { }

	MemoryController(const MemoryController &) = delete;

	MemoryController &operator= (const MemoryController &) = delete;

	// Unmaps the chunks of the pools. All memory that was allocated from them must be freed.
	~MemoryController();

	// Pages that user-class allocations of hierarchies using this controller leave free on top
	// of the user floor; see AllocationPolicy::floor.
	size_t floor() const {
		return floor_;
	}

	// Kernel memory that is allocated on behalf of the controller's nodes.
	// There is no limit on it yet.
	void chargeKernel(size_t bytes) {
		chargedKernel_.fetch_add(bytes, std::memory_order_relaxed);
	}

	void unchargeKernel(size_t bytes) {
		chargedKernel_.fetch_sub(bytes, std::memory_order_relaxed);
	}

	size_t chargedKernel() const {
		return chargedKernel_.load(std::memory_order_relaxed);
	}

	// The controller's own heap pool of the given CPU, which charges the pages that it maps.
	// Memory is freed through any pool (e.g., through Allocator).
	Pool *kernelPoolFor(CpuData *cpu) {
		return pools_[cpu->cpuIndex];
	}

private:
	size_t floor_{0};
	std::atomic<size_t> chargedKernel_{0};

	// Indexed by cpuIndex. Constructed by create().
	frg::dyn_array<Pool *, KernelAlloc> pools_;
};

// Charges the heap memory that it allocates to a memory controller.
// A null account is the kernel's own account, i.e., allocation through Allocator.
struct AccountAllocator {
	AccountAllocator() = default;

	explicit AccountAllocator(MemoryController *account)
	: account_{account} { }

	void *allocate(size_t size) const {
		if(!account_)
			return kernelAlloc->allocate(size);
		{
			Allocator::Guard guard;
			if(auto p = account_->kernelPoolFor(getCpuData())->allocate(size); p) [[likely]]
				return p;
		}
		return refillAndAllocate_(size);
	}

	// The controllers' pools are in the same shards as the kernel's pools, so any pool can free.
	void deallocate(void *p, size_t size) const {
		kernelAlloc->deallocate(p, size);
	}

	void free(void *p) const {
		kernelAlloc->free(p);
	}

private:
	// Slow path of allocate() if the pool needs a refill.
	void *refillAndAllocate_(size_t size) const;

	MemoryController *account_{nullptr};
};

} // namespace thor
