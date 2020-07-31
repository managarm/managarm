#include <thor-internal/core.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/ring-buffer.hpp>

// This is required for virtual destructors. It should not be called though.
void operator delete(void *, size_t) {
	frigg::panicLogger() << "thor: operator delete() called" << frigg::endLog;
}

namespace thor {

size_t kernelVirtualUsage = 0;
size_t kernelMemoryUsage = 0;

namespace {
	constexpr bool logCleanup = false;
}

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

KernelVirtualMemory::KernelVirtualMemory() {
	// The size is chosen arbitrarily here; 1 GiB of kernel heap is sufficient for now.
	uintptr_t vmBase = 0xFFFF'E000'0000'0000;
	size_t desiredSize = 0x4000'0000;

	// Setup a buddy allocator.
	auto tableOrder = BuddyAccessor::suitableOrder(desiredSize >> kPageShift);
	auto guessedRoots = desiredSize >> (kPageShift + tableOrder);
	auto overhead = BuddyAccessor::determineSize(guessedRoots, tableOrder);
	overhead = (overhead + (kPageSize - 1)) & ~(kPageSize - 1);

	size_t availableSize = desiredSize - overhead;
	auto availableRoots = availableSize >> (kPageShift + tableOrder);

	for(size_t pg = 0; pg < overhead; pg += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(0x1000);
		assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
		KernelPageSpace::global().mapSingle4k(vmBase + availableSize + pg, physical,
				page_access::write, CachingMode::null);
	}
	auto tablePtr = reinterpret_cast<int8_t *>(vmBase + availableSize);
	unpoisonKasanShadow(tablePtr, overhead);
	BuddyAccessor::initialize(tablePtr, availableRoots, tableOrder);

	buddy_ = BuddyAccessor{vmBase, kPageShift,
				tablePtr, availableRoots, tableOrder};
}

void *KernelVirtualMemory::allocate(size_t length) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	// TODO: use a smarter implementation here.
	int order = 0;
	while(length > (size_t{1} << (kPageShift + order)))
		++order;

	auto address = buddy_.allocate(order, 64);
	if(address == BuddyAccessor::illegalAddress) {
		frigg::infoLogger() << "thor: Failed to allocate 0x" << frigg::logHex(length)
				<< " bytes of kernel virtual memory" << frigg::endLog;
		frigg::infoLogger() << "thor:"
				" Physical usage: " << (physicalAllocator->numUsedPages() * 4) << " KiB,"
				" kernel VM: " << (kernelVirtualUsage / 1024) << " KiB"
				" kernel RSS: " << (kernelMemoryUsage / 1024) << " KiB"
				<< frigg::endLog;
		frigg::panicLogger() << "\e[31m" "thor: Out of kernel virtual memory" "\e[39m"
				<< frigg::endLog;
	}
	kernelVirtualUsage += (size_t{1} << (kPageShift + order));

	auto pointer = reinterpret_cast<void *>(address);
	unpoisonKasanShadow(pointer, size_t{1} << (kPageShift + order));

	return pointer;
}

void KernelVirtualMemory::deallocate(void *pointer, size_t length) {
	auto irqLock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&mutex_);

	// TODO: use a smarter implementation here.
	int order = 0;
	while(length > (size_t{1} << (kPageShift + order)))
		++order;

	poisonKasanShadow(pointer, size_t{1} << (kPageShift + order));
	buddy_.free(reinterpret_cast<uintptr_t>(pointer), order);
	assert(kernelVirtualUsage >= (size_t{1} << (kPageShift + order)));
	kernelVirtualUsage -= (size_t{1} << (kPageShift + order));
}

frigg::LazyInitializer<KernelVirtualMemory> kernelVirtualMemory;

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

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = KernelPageSpace::global().unmapSingle4k(address + offset);
		physicalAllocator->free(physical, kPageSize);
	}
	kernelMemoryUsage -= length;

	struct Closure {
		PhysicalAddr thisPage;
		size_t address;
		size_t length;
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
	p->length = length;
	p->worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		KernelVirtualMemory::global().deallocate(reinterpret_cast<void *>(closure->address),
				closure->length);
		auto physical = closure->thisPage;
		closure->~Closure();
		asm volatile ("" : : : "memory");
		physicalAllocator->free(physical, kPageSize);
	});
	p->shootNode.address = address;
	p->shootNode.size = length;
	p->shootNode.setup(&p->worklet);
	if(KernelPageSpace::global().submitShootdown(&p->shootNode))
		WorkQueue::post(&p->worklet);
}

frigg::LazyInitializer<LogRingBuffer> allocLog;

void KernelVirtualAlloc::output_trace(uint8_t val) {
	if (!allocLog)
		allocLog.initialize(0xFFFF'F000'0000'0000, 268435456);

	allocLog->enqueue(val);
}

frigg::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;

frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;

frigg::LazyInitializer<
	frg::slab_pool<
		KernelVirtualAlloc,
		IrqSpinlock
	>
> kernelHeap;

frigg::LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// CpuData
// --------------------------------------------------------

IrqMutex &irqMutex() {
	return getCpuData()->irqMutex;
}

ExecutorContext::ExecutorContext() { }

CpuData::CpuData()
: scheduler{this}, activeFiber{nullptr}, heartbeat{0} { }

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
: _descriptorMap{frg::hash<Handle>{}, *kernelAlloc}, _nextHandle{1} { }

Universe::~Universe() {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: Universe is deallocated\e[39m" << frigg::endLog;
}

Handle Universe::attachDescriptor(Guard &guard, AnyDescriptor descriptor) {
	assert(guard.protects(&lock));

	Handle handle = _nextHandle++;
	_descriptorMap.insert(handle, frigg::move(descriptor));
	return handle;
}

AnyDescriptor *Universe::getDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));

	return _descriptorMap.get(handle);
}

frg::optional<AnyDescriptor> Universe::detachDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));

	return _descriptorMap.remove(handle);
}

} // namespace thor

// --------------------------------------------------------
// Frigg glue functions
// --------------------------------------------------------

frigg::TicketLock logLock;

void friggBeginLog() {
	thor::irqMutex().lock();
	logLock.lock();
}

void friggEndLog() {
	logLock.unlock();
	thor::irqMutex().unlock();
}

void friggPrintCritical(char c) {
	thor::infoSink.print(c);
}
void friggPrintCritical(char const *str) {
	thor::infoSink.print(str);
}
void friggPanic() {
	thor::disableInts();
	while(true) {
		thor::halt();
	}
}
