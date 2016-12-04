
#include "kernel.hpp"

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

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

struct KernelVirtualMemory {
public:
	static KernelVirtualMemory &global();

	// TODO: make this private
	KernelVirtualMemory() {
		// the size is chosen arbitrarily here; 1 GiB of kernel heap is sufficient for now.
		uintptr_t original_base = 0xFFFF'8000'0000'0000;
		size_t original_size = 0x4000'0000;
		
		size_t fine_shift = kPageShift + 4, coarse_shift = kPageShift + 8;
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
			PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
			for(size_t offset = 0; offset < overhead; offset += kPageSize) {
				PhysicalAddr physical = physicalAllocator->allocate(physical_guard, 0x1000);
				kernelSpace->mapSingle4k(physical_guard, original_base + offset, physical, false,
						PageSpace::kAccessWrite);
			}
		}
		asm("" : : : "memory");
		thorRtInvalidateSpace();

		_buddy.addChunk(base, length, fine_shift, coarse_shift, (void *)original_base);
	}

	KernelVirtualMemory(const KernelVirtualMemory &other) = delete;
	
	KernelVirtualMemory &operator= (const KernelVirtualMemory &other) = delete;

	void *allocate(size_t length) {
		return (void *)_buddy.allocate(length);
	}

private:
	frigg::BuddyAllocator _buddy;
};

frigg::LazyInitializer<KernelVirtualMemory> kernelVirtualMemory;

KernelVirtualMemory &KernelVirtualMemory::global() {
	if(!kernelVirtualMemory)
		kernelVirtualMemory.initialize();
	return *kernelVirtualMemory;
}


KernelVirtualAlloc::KernelVirtualAlloc() { }

uintptr_t KernelVirtualAlloc::map(size_t length) {
	auto p = KernelVirtualMemory::global().allocate(length);

	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(physical_guard, 0x1000);
		kernelSpace->mapSingle4k(physical_guard, VirtualAddr(p) + offset, physical, false,
				PageSpace::kAccessWrite);
	}
	physical_guard.unlock();

	asm("" : : : "memory");
	thorRtInvalidateSpace();

	return uintptr_t(p);
}

void KernelVirtualAlloc::unmap(uintptr_t address, size_t length) {
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	asm("" : : : "memory");
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = kernelSpace->unmapSingle4k(address + offset);
		(void)physical;
//	TODO: reeneable this after fixing physical memory allocator
//		physicalAllocator->free(physical_guard, physical);
	}
	physical_guard.unlock();

	thorRtInvalidateSpace();
}

frigg::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
frigg::LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// CpuData class
// --------------------------------------------------------

CpuData::CpuData()
: context(nullptr) { }

// --------------------------------------------------------
// SubmitInfo
// --------------------------------------------------------

SubmitInfo::SubmitInfo()
: asyncId(0), submitFunction(0), submitObject(0) { }

SubmitInfo::SubmitInfo(int64_t async_id,
		uintptr_t submit_function, uintptr_t submit_object)
: asyncId(async_id), submitFunction(submit_function),
		submitObject(submit_object) { }

// --------------------------------------------------------
// BaseRequest
// --------------------------------------------------------

BaseRequest::BaseRequest(KernelSharedPtr<EventHub> event_hub, SubmitInfo submit_info)
: eventHub(frigg::move(event_hub)), submitInfo(submit_info) { }

// --------------------------------------------------------
// ThreadRunControl
// --------------------------------------------------------
		
void ThreadRunControl::increment() {
	int previous_ref_count;
	frigg::fetchInc(&_thread->_runCount, previous_ref_count);
	assert(previous_ref_count > 0);
}

void ThreadRunControl::decrement() {
	int previous_ref_count;
	frigg::fetchDec(&_thread->_runCount, previous_ref_count);
	if(previous_ref_count == 1) {
		// FIXME: protect this with a lock
		frigg::infoLogger() << "Make sure thread going out of scope works correctly"
				<< frigg::endLog;
		_thread->signalKill();
		_counter->decrement();
	}
}

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
: _descriptorMap(frigg::DefaultHasher<Handle>(), *kernelAlloc), _nextHandle(1) { }

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

frigg::Optional<AnyDescriptor> Universe::detachDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));
	
	return _descriptorMap.remove(handle);
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


