
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
frigg::LazyInitializer<frigg::DefaultLogger<BochsSink>> infoLogger;

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

KernelVirtualAlloc::KernelVirtualAlloc()
: p_nextPage(0xFFFF800200000000) { }

uintptr_t KernelVirtualAlloc::map(size_t length) {
	assert((length % kPageSize) == 0);
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
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	asm("" : : : "memory");
	PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = kernelSpace->unmapSingle4k(address + offset);
		physicalAllocator->free(physical_guard, physical);
	}
	physical_guard.unlock();

	thorRtInvalidateSpace();
}

frigg::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
frigg::LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// CpuContext class
// --------------------------------------------------------

CpuContext::CpuContext() {
	auto address_space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	auto thread = frigg::makeShared<Thread>(*kernelAlloc, KernelSharedPtr<Universe>(),
			frigg::move(address_space), KernelSharedPtr<RdFolder>());
	thread->flags |= Thread::kFlagNotScheduled;

	// FIXME: do not heap-allocate the state structs
	void *state = kernelAlloc->allocate(getStateSize());
	auto gpr_state = accessGprState(state);
	gpr_state->rsp = (uintptr_t)thread->accessSaveState().syscallStack
			+ ThorRtThreadState::kSyscallStackSize;
	gpr_state->rflags = 0x200; // enable interrupts
	gpr_state->rip = (Word)&idleRoutine;
	gpr_state->kernel = 1;
	thread->accessSaveState().restoreState = state;

	idleThread = thread;
	activeList->addBack(frigg::move(thread));
}

// --------------------------------------------------------
// SubmitInfo
// --------------------------------------------------------

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
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
: p_descriptorMap(frigg::DefaultHasher<Handle>(), *kernelAlloc),
		p_nextHandle(1) { }

Handle Universe::attachDescriptor(Guard &guard, AnyDescriptor &&descriptor) {
	assert(guard.protects(&lock));

	Handle handle = p_nextHandle++;
	p_descriptorMap.insert(handle, frigg::move(descriptor));
	return handle;
}

AnyDescriptor *Universe::getDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));

	return p_descriptorMap.get(handle);
}

frigg::Optional<AnyDescriptor> Universe::detachDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));
	
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


