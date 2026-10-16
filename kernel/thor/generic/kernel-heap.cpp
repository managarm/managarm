#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/arch-generic/paging.hpp>

namespace thor {

THOR_DEFINE_PERCPU_UNINITIALIZED(heapSlabPool);
THOR_DEFINE_PERCPU(inSlabPool);

namespace {

constinit std::atomic<size_t> kernelVirtualUsage{0};
constinit std::atomic<size_t> kernelCoreUsage{0};
constinit std::atomic<size_t> kernelHeapUsage{0};

constinit CoreSlabPolicy coreSlabPolicy;

frg::manual_box<
	frg::slab_pool<CoreSlabPolicy, IrqSpinlock>
> corePool;

} // namespace

uintptr_t CoreSlabPolicy::map(size_t size, size_t align) {
	assert(size <= kPageSize);
	assert(align <= kPageSize);
	PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
	assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
	kernelCoreUsage.fetch_add(kPageSize, std::memory_order_relaxed);
	return reinterpret_cast<uintptr_t>(mapDirectPhysical(physical));
}

void CoreSlabPolicy::unmap(uintptr_t address, size_t size) {
	(void)size;
	auto physical = reverseDirectPhysical(reinterpret_cast<void *>(address));
	physicalAllocator->free(physical, kPageSize);
	auto usage = kernelCoreUsage.fetch_sub(kPageSize, std::memory_order_relaxed);
	assert(usage >= kPageSize);
}

CoreAllocator &getCoreAllocator() {
	static CoreAllocator allocator{corePool.get()};
	return allocator;
}

namespace {

struct KernelVirtualHole {
	uintptr_t address = 0;
	size_t size = 0;
	frg::rbtree_hook treeHook;
	size_t largestHole = 0;
};

struct KernelVirtualLess {
	bool operator() (const KernelVirtualHole &a, const KernelVirtualHole &b) {
		return a.address < b.address;
	}
};

struct KernelVirtualAggregator;

using KernelVirtualTree = frg::rbtree<
	KernelVirtualHole,
	&KernelVirtualHole::treeHook,
	KernelVirtualLess,
	KernelVirtualAggregator
>;

struct KernelVirtualAggregator {
	static bool aggregate(KernelVirtualHole *node) {
		size_t size = node->size;
		if(auto left = KernelVirtualTree::get_left(node); left && left->largestHole > size)
			size = left->largestHole;
		if(auto right = KernelVirtualTree::get_right(node); right && right->largestHole > size)
			size = right->largestHole;

		if(node->largestHole == size)
			return false;
		node->largestHole = size;
		return true;
	}

	static bool check_invariant(KernelVirtualTree &, KernelVirtualHole *) {
		return true;
	}
};

frg::manual_box<KernelVirtualTree> virtualTree;

} // anonymous namespace.

KernelVirtualMemory::KernelVirtualMemory() {
	// The size is chosen arbitrarily here; 2 GiB of kernel heap is sufficient for now.
	uintptr_t vmBase = memoryLayoutNote->kernelVirtual;
	size_t desiredSize = memoryLayoutNote->kernelVirtualSize;

	corePool.initialize(coreSlabPolicy);
	virtualTree.initialize();

	auto initialHole = frg::construct<KernelVirtualHole>(getCoreAllocator());
	initialHole->address = vmBase;
	initialHole->size = desiredSize;
	initialHole->largestHole = desiredSize;
	virtualTree->insert(initialHole);
}

void *KernelVirtualMemory::allocate(size_t size) {
	// Round up to page size.
	size = (size + kPageSize - 1) & ~(kPageSize - 1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);
	void *pointer;
	{
		if(virtualTree->get_root()->largestHole < size) {
			infoLogger() << "thor: Failed to allocate 0x" << frg::hex_fmt(size)
					<< " bytes of kernel virtual memory" << frg::endlog;
			infoLogger() << "thor:"
					" Physical usage: " << (physicalAllocator->numUsedPages() * 4) << " KiB,"
					" kernel VM: " << (kernelVirtualUsage.load(std::memory_order_relaxed) / 1024) << " KiB"
					" kernel RSS: " << (kernelHeapUsage.load(std::memory_order_relaxed) / 1024) << " KiB"
					<< frg::endlog;
			panicLogger() << "thor: Out of kernel virtual memory" << frg::endlog;
		}

		auto current = virtualTree->get_root();
		while(true) {
			// Try to allocate memory at the bottom of the range.
			if(KernelVirtualTree::get_left(current)
					&& KernelVirtualTree::get_left(current)->largestHole >= size) {
				current = KernelVirtualTree::get_left(current);
				continue;
			}

			if(current->size >= size)
				break;

			assert(KernelVirtualTree::get_right(current));
			assert(KernelVirtualTree::get_right(current)->largestHole >= size);
			current = KernelVirtualTree::get_right(current);
		}

		// Remember the address before the hole might be deallocated.
		pointer = reinterpret_cast<void *>(current->address);
		virtualTree->remove(current);

		if(current->size == size) {
			frg::destruct(getCoreAllocator(), current);
		}else{
			assert(current->size > size);
			current->address += size;
			current->size -= size;
			virtualTree->insert(current);
		}
	}

	kernelVirtualUsage.fetch_add(size, std::memory_order_relaxed);
	unpoisonKasanShadow(pointer, size);
	return pointer;
}

void KernelVirtualMemory::deallocate(void *pointer, size_t size) {
	// Round up to page size.
	size = (size + kPageSize - 1) & ~(kPageSize - 1);

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);
	{
		auto hole = frg::construct<KernelVirtualHole>(getCoreAllocator());
		hole->address = reinterpret_cast<uintptr_t>(pointer);
		hole->size = size;
		hole->largestHole = size;
		virtualTree->insert(hole);
	}

	auto usage = kernelVirtualUsage.fetch_sub(size, std::memory_order_relaxed);
	assert(usage >= size);
	poisonKasanShadow(pointer, size);
}

frg::manual_box<KernelVirtualMemory> kernelVirtualMemory;

KernelVirtualMemory &KernelVirtualMemory::global() {
	// TODO: This should be initialized at a well-defined stage in the
	// kernel's boot process.
	if(!kernelVirtualMemory)
		kernelVirtualMemory.initialize();
	return *kernelVirtualMemory;
}

void *HeapSlabPolicy::map(size_t length) {
	assert(!(length % kPageSize));
	size_t numPages = length / kPageSize;

	auto ctx = currentExecutorContext();
	HeapMagazine scratch;
	auto &magazine = ctx ? ctx->heapMagazine : scratch;
	if(magazine.pages.size() < numPages) {
		// Contexts without an ExecutorContext cannot sleep, so they use the emergency class.
		// Other contexts try to allocate here and let refillAndAllocate_() deal with failure.
		auto cls = ctx ? AllocationClass::kernel : AllocationClass::emergency;
		auto deficit = numPages - magazine.pages.size();
		auto pages = physicalAllocator->tryAllocateChain(cls, PhysicalKind::pinned, deficit);
		if(!pages) {
			if(!ctx)
				panicLogger() << "thor: Out of memory for the kernel heap" << frg::endlog;
			magazine.wanted = deficit;
			return nullptr;
		}
		magazine.pages.splice(std::move(*pages));
	}

	auto p = KernelVirtualMemory::global().allocate(length);

	// TODO: The slab_pool unpoisons memory before calling this.
	//       It would be better not to unpoison in the kernel's VMM code.
	poisonKasanShadow(p, length);

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		KernelPageSpace::global().mapSingle4k(VirtualAddr(p) + offset, magazine.pages.pop(),
				page_access::write, CachingMode::null);
	}
	kernelHeapUsage.fetch_add(length, std::memory_order_relaxed);

	return p;
}

void HeapSlabPolicy::unmap(void *ptr, size_t length) {
	auto address = reinterpret_cast<uintptr_t>(ptr);
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	// TODO: The slab_pool poisons memory before calling this.
	//       It would be better not to poison in the kernel's VMM code.
	unpoisonKasanShadow(reinterpret_cast<void *>(address), length);

	PageChain pages;
	for(size_t offset = 0; offset < length; offset += kPageSize)
		pages.push(KernelPageSpace::global().unmapSingle4k(address + offset));

	spawnOnWorkQueue(
		getCoreAllocator(),
		WorkQueue::generalQueue().lock(),
		async::transform(
			shootdown(
				&KernelPageSpace::global(),
				address,
				length,
				WorkQueue::generalQueue().get()
			),
			[address, length, pages = std::move(pages)] mutable {
				while(!pages.empty())
					physicalAllocator->free(pages.pop(), kPageSize);
				KernelVirtualMemory::global().deallocate(reinterpret_cast<void *>(address), length);
				auto usage = kernelHeapUsage.fetch_sub(length, std::memory_order_relaxed);
				assert(usage >= length);
			}
		)
	);
}

HeapMagazine::~HeapMagazine() {
	while(!pages.empty())
		physicalAllocator->free(pages.pop(), kPageSize);
}

void *Allocator::refillAndAllocate_(size_t size) const {
	// map() panics instead of failing if there is no ExecutorContext.
	auto ctx = currentExecutorContext();
	if(!ctx)
		panicLogger() << "thor: Kernel heap allocation failed outside of ExecutorContext" << frg::endlog;
	auto &magazine = ctx->heapMagazine;
	auto canBlock = canBlockForMemory();

	while(true) {
		// Read wanted before blocking since blocking can allocate (and fail) in this context.
		auto wanted = magazine.wanted;
		magazine.wanted = 0;
		// The pool failed without calling map() (e.g., due to an overflowing size).
		if(!wanted)
			return nullptr;

		PageChain refill;
		while(canBlock && refill.size() < wanted) {
			auto batch = frg::min(wanted - refill.size(), PhysicalChunkAllocator::maxPagesPerWait);
			auto pages = physicalAllocator->allocateChainBlocking(
				AllocationClass::kernel, PhysicalKind::pinned, batch
			);
			// If blocking failed, fall back to the emergency tryAllocateChain() below.
			if(!pages)
				break;
			refill.splice(std::move(*pages));
		}
		// Try the emergency class if we either cannot block or if blocking failed above.
		if(refill.size() < wanted) {
			auto pages = physicalAllocator->tryAllocateChain(AllocationClass::emergency,
					PhysicalKind::pinned, wanted - refill.size());
			if(!pages)
				panicLogger() << "thor: Out of memory for the kernel heap" << frg::endlog;
			refill.splice(std::move(*pages));
		}

		Guard guard;
		// Retry the allocation after filling the magazine.
		magazine.pages.splice(std::move(refill));
		if(auto p = heapSlabPool.get().allocate(size); p)
			return p;
	}
}

frg::manual_box<LogRingBuffer> allocLog;

namespace {
	initgraph::Task initAllocTraceSink{&globalInitEngine, "generic.init-alloc-trace-sink",
		initgraph::Requires{getFibersAvailableStage(),
			getIoChannelsDiscoveredStage()},
		[] {
#ifndef KERNEL_LOG_ALLOCATIONS
			return;
#endif // KERNEL_LOG_ALLOCATIONS

			spawnOnWorkQueue(*kernelAlloc, WorkQueue::generalQueue().lock(),
					dumpRingToChannel(allocLog.get(), "kernel-alloc-trace", 2048));
		}
	};
}

void HeapSlabPolicy::unpoison(void *pointer, size_t size) {
	unpoisonKasanShadow(pointer, size);
}

void HeapSlabPolicy::unpoison_expand(void *pointer, size_t size) {
	cleanKasanShadow(pointer, size);
}

void HeapSlabPolicy::poison(void *pointer, size_t size) {
	poisonKasanShadow(pointer, size);
}

void HeapSlabPolicy::output_trace(void *buffer, size_t size) {
	if (!allocLog)
		allocLog.initialize(memoryLayoutNote->allocLog, memoryLayoutNote->allocLogSize);

	allocLog->enqueue(buffer, size);
}

constinit frg::manual_box<PhysicalChunkAllocator> physicalAllocator = {};

constinit frg::manual_box<KernelAlloc> kernelAlloc = {};

} // namespace thor
