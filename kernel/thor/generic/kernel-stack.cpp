#include <thor-internal/arch/paging.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

UniqueKernelStack UniqueKernelStack::make() {
	size_t guardedSize = kSize + kPageSize;
	auto pointer = KernelVirtualMemory::global().allocate(guardedSize);

	for(size_t offset = 0; offset < kSize; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
		KernelPageSpace::global().mapSingle4k(
				reinterpret_cast<VirtualAddr>(pointer) + guardedSize - kSize + offset,
				physical, page_access::write, CachingMode::null);
	}

	return UniqueKernelStack(reinterpret_cast<char *>(pointer) + guardedSize);
}

UniqueKernelStack::~UniqueKernelStack() {
	if(!_base)
		return;

	size_t guardedSize = kSize + kPageSize;
	auto address = reinterpret_cast<uintptr_t>(_base - guardedSize);
	for(size_t offset = 0; offset < kSize; offset += kPageSize) {
		PhysicalAddr physical = KernelPageSpace::global().unmapSingle4k(
				address + guardedSize - kSize + offset);
		physicalAllocator->free(physical, kPageSize);
	}

	struct Closure : ShootNode {
		void complete() override {
			KernelVirtualMemory::global().deallocate(reinterpret_cast<void *>(address), size);
			auto physical = thisPage;
			Closure::~Closure();
			asm volatile ("" : : : "memory");
			physicalAllocator->free(physical, kPageSize);
		}

		PhysicalAddr thisPage;
	};
	static_assert(sizeof(Closure) <= kPageSize);

	// We need some memory to store the closure that waits until shootdown completes.
	// For now, our stategy consists of allocating one page of *physical* memory
	// and accessing it through the global physical mapping.
	auto physical = physicalAllocator->allocate(kPageSize);
	PageAccessor accessor{physical};
	auto p = new (accessor.get()) Closure;
	p->thisPage = physical;
	p->address = address;
	p->size = guardedSize;
	if(KernelPageSpace::global().submitShootdown(p))
		p->complete();
}

} //namespace thor
