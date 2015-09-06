
#include "kernel.hpp"

namespace traits = frigg::traits;
namespace util = frigg::util;

namespace thor {

static int64_t nextAsyncId = 1;

int64_t allocAsyncId() {
	int64_t async_id;
	frigg::fetchInc<int64_t>(&nextAsyncId, async_id);
	return async_id;
}

// --------------------------------------------------------
// Debugging and logging
// --------------------------------------------------------

BochsSink infoSink;
frigg::util::LazyInitializer<frigg::debug::DefaultLogger<BochsSink>> infoLogger;

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

KernelVirtualAlloc::KernelVirtualAlloc()
: p_nextPage(0xFFFF800200000000) { }

uintptr_t KernelVirtualAlloc::map(size_t length) {
	ASSERT((length % kPageSize) == 0);
	uintptr_t address = p_nextPage;
	p_nextPage += length;

	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, 1);
		kernelSpace->mapSingle4k(physical_guard, address + offset, physical, false,
				PageSpace::kAccessWrite);
	}
	physical_guard.unlock();

	asm("" : : : "memory");
	thorRtInvalidateSpace();

	return address;
}

void KernelVirtualAlloc::unmap(uintptr_t address, size_t length) {
	ASSERT((address % kPageSize) == 0);
	ASSERT((length % kPageSize) == 0);

	asm("" : : : "memory");
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = kernelSpace->unmapSingle4k(address + offset);
		physicalAllocator->free(physical_guard, physical);
	}
	physical_guard.unlock();

	thorRtInvalidateSpace();
}

frigg::util::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
frigg::util::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
frigg::util::LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
		: p_descriptorMap(util::DefaultHasher<Handle>(), *kernelAlloc) { }

Handle Universe::attachDescriptor(Guard &guard, AnyDescriptor &&descriptor) {
	ASSERT(guard.protects(&lock));

	Handle handle = p_nextHandle++;
	p_descriptorMap.insert(handle, traits::move(descriptor));
	return handle;
}

frigg::Optional<AnyDescriptor *> Universe::getDescriptor(Guard &guard, Handle handle) {
	ASSERT(guard.protects(&lock));

	return p_descriptorMap.get(handle);
}

AnyDescriptor Universe::detachDescriptor(Guard &guard, Handle handle) {
	ASSERT(guard.protects(&lock));

	return p_descriptorMap.remove(handle);
}

} // namespace thor

// --------------------------------------------------------
// Frigg glue functions
// --------------------------------------------------------

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


