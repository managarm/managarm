#ifndef THOR_GENERIC_KERNEL_HEAP_HPP
#define THOR_GENERIC_KERNEL_HEAP_HPP

#include <frigg/atomic.hpp>
#include <frigg/initializer.hpp>
#include <frigg/memory-slab.hpp>
#include <frigg/physical_buddy.hpp>

namespace thor {

struct KernelVirtualMemory {
public:
	static KernelVirtualMemory &global();

	// TODO: make this private
	KernelVirtualMemory();

	KernelVirtualMemory(const KernelVirtualMemory &other) = delete;
	
	KernelVirtualMemory &operator= (const KernelVirtualMemory &other) = delete;

	void *allocate(size_t length);

private:
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

private:
	frigg::SlabAllocator<KernelVirtualAlloc, frigg::TicketLock> _allocator;
};

extern frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
extern frigg::LazyInitializer<KernelAlloc> kernelAlloc;

} // namespace thor

#endif // THOR_GENERIC_KERNEL_HEAP_HPP
