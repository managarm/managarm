#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <thor-internal/arch-generic/paging.hpp>

// This is required for virtual destructors. It should not be called though.
void operator delete(void *, size_t) {
	thor::panicLogger() << "thor: operator delete() called" << frg::endlog;
}

namespace thor {

size_t kernelVirtualUsage = 0;
size_t kernelMemoryUsage = 0;

// --------------------------------------------------------
// Locking primitives
// --------------------------------------------------------

void IrqSpinlock::lock() {
	irqMutex().lock();
	_spinlock.lock();
}

void IrqSpinlock::unlock() {
	_spinlock.unlock();
	irqMutex().unlock();
}

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

namespace {

struct CoreSlabPolicy {
	static constexpr size_t sb_size = kPageSize;
	static constexpr size_t slabsize = kPageSize;

	uintptr_t map(size_t size, size_t align) {
		assert(size <= kPageSize);
		assert(align <= kPageSize);
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
		return reinterpret_cast<uintptr_t>(mapDirectPhysical(physical));
	}

	void unmap(uintptr_t address, size_t size) {
		(void)size;
		auto physical = reverseDirectPhysical(reinterpret_cast<void *>(address));
		physicalAllocator->free(physical, kPageSize);
	}
};

constinit CoreSlabPolicy coreSlabPolicy;

frg::manual_box<
	frg::slab_pool<CoreSlabPolicy, IrqSpinlock>
> corePool;

// TODO: we do not really want to return a mutable reference here,
//       but frg::construct requires it for now.
frg::slab_allocator<CoreSlabPolicy, IrqSpinlock> &getCoreAllocator() {
	static frg::slab_allocator<CoreSlabPolicy, IrqSpinlock> allocator{corePool.get()};
	return allocator;
}

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
					" kernel VM: " << (kernelVirtualUsage / 1024) << " KiB"
					" kernel RSS: " << (kernelMemoryUsage / 1024) << " KiB"
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

	kernelVirtualUsage += size; // FIXME: atomicity.
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

	assert(kernelVirtualUsage >= size);
	kernelVirtualUsage -= size;
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

KernelVirtualAlloc::KernelVirtualAlloc() { }

uintptr_t KernelVirtualAlloc::map(size_t length) {
	auto p = KernelVirtualMemory::global().allocate(length);

	// TODO: The slab_pool unpoisons memory before calling this.
	//       It would be better not to unpoison in the kernel's VMM code.
	poisonKasanShadow(p, length);

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(kPageSize);
		assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
		KernelPageSpace::global().mapSingle4k(VirtualAddr(p) + offset, physical,
				page_access::write, CachingMode::null);
	}
	kernelMemoryUsage += length;

	return uintptr_t(p);
}

void KernelVirtualAlloc::unmap(uintptr_t address, size_t length) {
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	// TODO: The slab_pool poisons memory before calling this.
	//       It would be better not to poison in the kernel's VMM code.
	unpoisonKasanShadow(reinterpret_cast<void *>(address), length);

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = KernelPageSpace::global().unmapSingle4k(address + offset);
		physicalAllocator->free(physical, kPageSize);
	}
	kernelMemoryUsage -= length;

	// TODO: we could replace this closure by an appropriate async::detach_with_allocator call.
	struct Closure final : ShootNode {
		void complete() override {
			frg::slab_allocator<CoreSlabPolicy, IrqSpinlock> coreAllocator(corePool.get());

			KernelVirtualMemory::global().deallocate(reinterpret_cast<void *>(address), size);
			Closure::~Closure();
			asm volatile ("" : : : "memory");
			frg::destruct(getCoreAllocator(), this);
		}
	};
	static_assert(sizeof(Closure) <= kPageSize);

	auto p = frg::construct<Closure>(getCoreAllocator());
	p->address = address;
	p->size = length;
	if(KernelPageSpace::global().submitShootdown(p))
		p->complete();
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

			auto channel = solicitIoChannel("kernel-alloc-trace");
			if(channel) {
				infoLogger() << "thor: Connecting alloc-trace to I/O channel" << frg::endlog;
				async::detach_with_allocator(*kernelAlloc,
						dumpRingToChannel(allocLog.get(), std::move(channel), 2048));
			}
		}
	};
}

void KernelVirtualAlloc::unpoison(void *pointer, size_t size) {
	unpoisonKasanShadow(pointer, size);
}

void KernelVirtualAlloc::unpoison_expand(void *pointer, size_t size) {
	cleanKasanShadow(pointer, size);
}

void KernelVirtualAlloc::poison(void *pointer, size_t size) {
	poisonKasanShadow(pointer, size);
}

void KernelVirtualAlloc::output_trace(void *buffer, size_t size) {
	if (!allocLog)
		allocLog.initialize(memoryLayoutNote->allocLog, memoryLayoutNote->allocLogSize);

	allocLog->enqueue(buffer, size);
}

constinit frg::manual_box<PhysicalChunkAllocator> physicalAllocator = {};

constinit frg::manual_box<KernelVirtualAlloc> kernelVirtualAlloc = {};

constinit frg::manual_box<
	frg::slab_pool<
		KernelVirtualAlloc,
		IrqSpinlock
	>
> kernelHeap = {};

constinit frg::manual_box<KernelAlloc> kernelAlloc = {};

// --------------------------------------------------------
// CpuData
// --------------------------------------------------------

ExecutorContext::ExecutorContext() { }

CpuData::CpuData()
: scheduler{this}, activeFiber{nullptr}, heartbeat{0} {
	iseqPtr = &regularIseq;
}

} // namespace thor
