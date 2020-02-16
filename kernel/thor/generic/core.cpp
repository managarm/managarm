
#include "kernel.hpp"

// This is required for virtual destructors. It should not be called though.
void operator delete(void *, size_t) {
	frigg::panicLogger() << "thor: operator delete() called" << frigg::endLog;
}

namespace thor {

size_t kernelMemoryUsage = 0;

namespace {
	constexpr bool logCleanup = false;
}

// --------------------------------------------------------
// Debugging and logging
// --------------------------------------------------------

BochsSink infoSink;

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
	// the size is chosen arbitrarily here; 1 GiB of kernel heap is sufficient for now.
	uintptr_t original_base = 0xFFFF'E000'0000'0000;
	size_t original_size = 0x4000'0000;
	
	size_t fine_shift = kPageShift + 4, coarse_shift = kPageShift + 12;
	size_t overhead = frigg::BuddyAllocator::computeOverhead(original_size,
			fine_shift, coarse_shift);
	
	uintptr_t base = original_base + overhead;
	size_t length = original_size - overhead;

	// align the base to the next coarse boundary.
	uintptr_t misalign = base % (uintptr_t(1) << coarse_shift);
	if(misalign) {
		base += (uintptr_t(1) << coarse_shift) - misalign;
		length -= misalign;
	}

	// shrink the length to the next coarse boundary.
	length -= length % (size_t(1) << coarse_shift);

	frigg::infoLogger() << "Kernel virtual memory overhead: 0x"
			<< frigg::logHex(overhead) << frigg::endLog;
	{
		for(size_t offset = 0; offset < overhead; offset += kPageSize) {
			PhysicalAddr physical = physicalAllocator->allocate(0x1000);
			assert(physical != static_cast<PhysicalAddr>(-1) && "OOM");
			KernelPageSpace::global().mapSingle4k(original_base + offset, physical,
					page_access::write, CachingMode::null);
		}
	}

	_buddy.addChunk(base, length, fine_shift, coarse_shift, (void *)original_base);
}

void *KernelVirtualMemory::allocate(size_t length) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	return (void *)_buddy.allocate(length);
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

	// TODO: Perform proper shootdown here.
	for(size_t offset = 0; offset < length; offset += kPageSize)
		invalidatePage(reinterpret_cast<char *>(address) + offset);
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


