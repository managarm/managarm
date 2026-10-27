#include <async/algorithm.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

UniqueKernelStack UniqueKernelStack::make() {
	size_t guardedSize = kSize + kPageSize;
	auto pointer = KernelVirtualMemory::global().allocate(guardedSize);

	for(size_t offset = 0; offset < kSize; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
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

	PageChain pages;
	for(size_t offset = 0; offset < kSize; offset += kPageSize)
		pages.push(KernelPageSpace::global().unmapSingle4k(
				address + guardedSize - kSize + offset));

	spawnOnWorkQueue(
		*kernelAlloc,
		WorkQueue::criticalQueue().lock(),
		async::transform(
			shootdown(
				&KernelPageSpace::global(),
				address,
				guardedSize,
				WorkQueue::criticalQueue().get()
			),
			[address, guardedSize, pages = std::move(pages)] mutable {
				while(!pages.empty())
					physicalAllocator->free(pages.pop(), kPageSize);
				KernelVirtualMemory::global().deallocate(
						reinterpret_cast<void *>(address), guardedSize);
			}
		)
	);
}

} //namespace thor
