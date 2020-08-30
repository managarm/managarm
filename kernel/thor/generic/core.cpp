#include <thor-internal/core.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/kasan.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/ring-buffer.hpp>

// This is required for virtual destructors. It should not be called though.
void operator delete(void *, size_t) {
	thor::panicLogger() << "thor: operator delete() called" << frg::endlog;
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
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

	// TODO: use a smarter implementation here.
	int order = 0;
	while(length > (size_t{1} << (kPageShift + order)))
		++order;

	auto address = buddy_.allocate(order, 64);
	if(address == BuddyAccessor::illegalAddress) {
		infoLogger() << "thor: Failed to allocate 0x" << frg::hex_fmt(length)
				<< " bytes of kernel virtual memory" << frg::endlog;
		infoLogger() << "thor:"
				" Physical usage: " << (physicalAllocator->numUsedPages() * 4) << " KiB,"
				" kernel VM: " << (kernelVirtualUsage / 1024) << " KiB"
				" kernel RSS: " << (kernelMemoryUsage / 1024) << " KiB"
				<< frg::endlog;
		panicLogger() << "\e[31m" "thor: Out of kernel virtual memory" "\e[39m"
				<< frg::endlog;
	}
	kernelVirtualUsage += (size_t{1} << (kPageShift + order));

	auto pointer = reinterpret_cast<void *>(address);
	unpoisonKasanShadow(pointer, size_t{1} << (kPageShift + order));

	return pointer;
}

void KernelVirtualMemory::deallocate(void *pointer, size_t length) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&mutex_);

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
	p->size = length;
	if(KernelPageSpace::global().submitShootdown(p))
		p->complete();
}

frigg::LazyInitializer<LogRingBuffer> allocLog;

void KernelVirtualAlloc::unpoison(void *pointer, size_t size) {
	unpoisonKasanShadow(pointer, size);
}

void KernelVirtualAlloc::unpoison_expand(void *pointer, size_t size) {
	cleanKasanShadow(pointer, size);
}

void KernelVirtualAlloc::poison(void *pointer, size_t size) {
	poisonKasanShadow(pointer, size);
}

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
		infoLogger() << "\e[31mthor: Universe is deallocated\e[39m" << frg::endlog;
}

Handle Universe::attachDescriptor(Guard &guard, AnyDescriptor descriptor) {
	assert(guard.protects(&lock));

	Handle handle = _nextHandle++;
	_descriptorMap.insert(handle, std::move(descriptor));
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

frg::ticket_spinlock logLock;

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
