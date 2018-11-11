#ifndef THOR_GENERIC_KERNEL_HEAP_HPP
#define THOR_GENERIC_KERNEL_HEAP_HPP

#include <frigg/atomic.hpp>
#include <frigg/initializer.hpp>
#include <frigg/physical_buddy.hpp>
#include <frg/slab.hpp>

namespace thor {

struct IrqSpinlock {
	void lock();
	void unlock();

private:
	frigg::TicketLock _spinlock;
};

struct KernelVirtualMemory {
	using Mutex = frigg::TicketLock;
public:
	static KernelVirtualMemory &global();

	// TODO: make this private
	KernelVirtualMemory();

	KernelVirtualMemory(const KernelVirtualMemory &other) = delete;
	
	KernelVirtualMemory &operator= (const KernelVirtualMemory &other) = delete;

	void *allocate(size_t length);

private:
	Mutex _mutex;
	frigg::BuddyAllocator _buddy;
};

class KernelVirtualAlloc {
public:
	KernelVirtualAlloc();

	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);
};

struct KernelAlloc {
	KernelAlloc(KernelVirtualAlloc &policy)
	: _allocator{policy} { }

	void *allocate(size_t size);
	void free(void *pointer);
	void deallocate(void *pointer, size_t size);

private:
	frg::slab_allocator<KernelVirtualAlloc, IrqSpinlock> _allocator;
};

extern frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
extern frigg::LazyInitializer<KernelAlloc> kernelAlloc;

} // namespace thor

#endif // THOR_GENERIC_KERNEL_HEAP_HPP
