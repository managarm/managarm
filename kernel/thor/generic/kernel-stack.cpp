#include <thor-internal/arch/paging.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <thor-internal/physical.hpp>

namespace thor {

constexpr size_t guardedSize = 0x10000;

UniqueKernelStack UniqueKernelStack::make() {
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

	auto address = reinterpret_cast<uintptr_t>(_base - guardedSize);
	for(size_t offset = 0; offset < kSize; offset += kPageSize) {
		PhysicalAddr physical = KernelPageSpace::global().unmapSingle4k(
				address + guardedSize - kSize + offset);
		physicalAllocator->free(physical, kPageSize);
	}

	struct Closure {
		PhysicalAddr thisPage;
		uintptr_t address;
		size_t size;
		Worklet worklet;
		ShootNode shootNode;
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
	p->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		KernelVirtualMemory::global().deallocate(reinterpret_cast<void *>(closure->address),
				closure->size);
		auto physical = closure->thisPage;
		closure->~Closure();
		asm volatile ("" : : : "memory");
		physicalAllocator->free(physical, kPageSize);
	});
	p->shootNode.address = address;
	p->shootNode.size = guardedSize;
	p->shootNode.setup(&p->worklet);
	if(KernelPageSpace::global().submitShootdown(&p->shootNode))
		WorkQueue::post(&p->worklet);
}

} //namespace thor
