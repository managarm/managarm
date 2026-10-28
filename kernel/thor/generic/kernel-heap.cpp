#include <bit>

#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/memory-controller.hpp>
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
	PhysicalAddr physical = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
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
	// Largest order of any naturally aligned power-of-two block that fits into a hole of this subtree.
	int largestOrder = 0;
};

// Order of the largest naturally aligned power-of-two block that fits into the given range.
int orderOfRange(uintptr_t address, size_t size) {
	assert(size);
	int order = std::bit_width(size) - 1;
	// If the 2^order block does not fit, a 2^(order - 1) block does since size >= 2^order.
	size_t block = size_t{1} << order;
	if(((-address) & (block - 1)) > size - block)
		return order - 1;
	return order;
}

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
		int order = orderOfRange(node->address, node->size);
		if(auto left = KernelVirtualTree::get_left(node); left && left->largestOrder > order)
			order = left->largestOrder;
		if(auto right = KernelVirtualTree::get_right(node); right && right->largestOrder > order)
			order = right->largestOrder;

		if(node->largestOrder == order)
			return false;
		node->largestOrder = order;
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
	virtualTree->insert(initialHole);
}

void *KernelVirtualMemory::allocate(size_t size) {
	return allocate(size, kPageSize);
}

void *KernelVirtualMemory::allocate(size_t size, size_t align) {
	assert(std::has_single_bit(align));
	// Round up to page size.
	size = (size + kPageSize - 1) & ~(kPageSize - 1);
	align = frg::max(align, size_t{kPageSize});

	// Search for a hole that contains a naturally aligned power-of-two block covering the request.
	// This is exact for naturally aligned power-of-two requests (such as sharded_slab chunks);
	// other requests are rounded up to the next power of two.
	int order = std::countr_zero(std::bit_ceil(frg::max(size, align)));
	size_t block = size_t{1} << order;

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);
	void *pointer;
	{
		if(!virtualTree->get_root() || virtualTree->get_root()->largestOrder < order) {
			// TODO: Before giving up, fall back to a linear search for a hole that fits without rounding.
			infoLogger() << "thor: Failed to allocate 0x" << frg::hex_fmt(size)
					<< " bytes of kernel virtual memory with alignment 0x" << frg::hex_fmt(align)
					<< " (order " << order << ")" << frg::endlog;
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
					&& KernelVirtualTree::get_left(current)->largestOrder >= order) {
				current = KernelVirtualTree::get_left(current);
				continue;
			}

			if(orderOfRange(current->address, current->size) >= order)
				break;

			assert(KernelVirtualTree::get_right(current));
			assert(KernelVirtualTree::get_right(current)->largestOrder >= order);
			current = KernelVirtualTree::get_right(current);
		}

		// The hole contains an aligned 2^order block, so the first such block fits.
		// Placing the request at a block boundary lets the same request reuse the range once it is freed.
		auto address = (current->address + block - 1) & ~(block - 1);
		pointer = reinterpret_cast<void *>(address);

		// Work with offsets since the end of the hole may not be representable.
		auto prefixSize = address - current->address;
		assert(prefixSize + size <= current->size);
		auto suffixSize = current->size - prefixSize - size;
		// Shrinking a hole in place keeps its position in the tree, so only the aggregates need updating.
		if(prefixSize) {
			current->size = prefixSize;
			virtualTree->aggregate_path(current);
			if(suffixSize) {
				// TODO: Allocate the hole before taking the locks. This only helps once the kernel heap
				//       stops calling into the virtual allocator from non-sleepable contexts.
				auto suffix = frg::construct<KernelVirtualHole>(getCoreAllocator());
				suffix->address = address + size;
				suffix->size = suffixSize;
				virtualTree->insert(suffix);
			}
		}else if(suffixSize) {
			current->address = address + size;
			current->size = suffixSize;
			virtualTree->aggregate_path(current);
		}else{
			virtualTree->remove(current);
			frg::destruct(getCoreAllocator(), current);
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
		auto address = reinterpret_cast<uintptr_t>(pointer);

		// Find the holes directly below and above the freed range.
		KernelVirtualHole *previous = nullptr;
		auto current = virtualTree->get_root();
		while(current) {
			if(current->address < address) {
				previous = current;
				current = KernelVirtualTree::get_right(current);
			}else{
				current = KernelVirtualTree::get_left(current);
			}
		}
		auto next = previous ? KernelVirtualTree::successor(previous) : virtualTree->first();
		assert(!previous || previous->address + previous->size <= address);
		assert(!next || address + size <= next->address);

		// Merge with adjacent holes such that the range can be handed out in one piece again.
		// Growing a hole in place keeps its position in the tree, so only the aggregates need updating.
		bool mergePrevious = previous && previous->address + previous->size == address;
		bool mergeNext = next && address + size == next->address;
		if(mergePrevious && mergeNext) {
			auto nextSize = next->size;
			virtualTree->remove(next);
			frg::destruct(getCoreAllocator(), next);
			previous->size += size + nextSize;
			virtualTree->aggregate_path(previous);
		}else if(mergePrevious) {
			previous->size += size;
			virtualTree->aggregate_path(previous);
		}else if(mergeNext) {
			next->address = address;
			next->size += size;
			virtualTree->aggregate_path(next);
		}else{
			// TODO: Allocate the hole before taking the locks. This only helps once the kernel heap
			//       stops calling into the virtual allocator from non-sleepable contexts.
			auto hole = frg::construct<KernelVirtualHole>(getCoreAllocator());
			hole->address = address;
			hole->size = size;
			virtualTree->insert(hole);
		}
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

void *HeapSlabPolicy::map(size_t length, size_t align) {
	assert(!(length % kPageSize));
	size_t numPages = length / kPageSize;

	auto ctx = currentExecutorContext();
	PageMagazine scratch;
	auto &magazine = ctx ? ctx->heapMagazine : scratch;
	if(magazine.pages.size() < numPages) {
		// Contexts without an ExecutorContext cannot sleep, so they use the emergency class.
		// Other contexts try to allocate here and let refillAndAllocate_() deal with failure.
		auto cls = ctx ? AllocationClass::kernel : AllocationClass::emergency;
		auto deficit = numPages - magazine.pages.size();
		auto pages = physicalAllocator->tryAllocateChain({cls, 0}, PhysicalKind::pinned, deficit);
		if(!pages) {
			if(!ctx)
				panicLogger() << "thor: Out of memory for the kernel heap" << frg::endlog;
			magazine.wanted = deficit;
			return nullptr;
		}
		magazine.pages.splice(std::move(*pages));
	}

	if(account)
		account->chargeKernel(length);

	auto p = KernelVirtualMemory::global().allocate(length, align);

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

	// Uncharge synchronously since the account can be gone once the shootdown completes.
	if(account)
		account->unchargeKernel(length);

	spawnOnWorkQueue(
		getCoreAllocator(),
		WorkQueue::criticalQueue().lock(),
		async::transform(
			shootdown(
				&KernelPageSpace::global(),
				address,
				length,
				WorkQueue::criticalQueue().get()
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

bool refillHeapMagazine() {
	// map() panics instead of failing if there is no ExecutorContext.
	auto ctx = currentExecutorContext();
	if(!ctx)
		panicLogger() << "thor: Kernel heap allocation failed outside of ExecutorContext" << frg::endlog;
	auto &magazine = ctx->heapMagazine;

	// The pool failed without calling map() (e.g., due to an overflowing size).
	if(!magazine.wanted)
		return false;

	if(auto refilled = magazine.refill(); !refilled)
		panicLogger() << "thor: Out of memory for the kernel heap" << frg::endlog;
	return true;
}

void *Allocator::refillAndAllocate_(size_t size) const {
	while(true) {
		if(!refillHeapMagazine())
			return nullptr;

		// Retry the allocation after filling the magazine.
		Guard guard;
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
