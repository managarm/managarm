#ifndef THOR_GENERIC_KERNEL_HEAP_HPP
#define THOR_GENERIC_KERNEL_HEAP_HPP

#include <frigg/physical_buddy.hpp>
#include <frigg/initializer.hpp>

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

typedef frigg::SlabAllocator<KernelVirtualAlloc, frigg::TicketLock> KernelAlloc;
extern frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
extern frigg::LazyInitializer<KernelAlloc> kernelAlloc;

} // namespace thor

#endif // THOR_GENERIC_KERNEL_HEAP_HPP
