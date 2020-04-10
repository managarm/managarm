#include <string.h>

#include <frg/container_of.hpp>
#include "event.hpp"
#include "execution/coroutine.hpp"
#include "kernel.hpp"
#include "ipc-queue.hpp"
#include "irq.hpp"
#include "kernlet.hpp"
#include "../arch/x86/debug.hpp"
#include <arch/x86/ept.hpp>
#include <arch/x86/vmx.hpp>
#include "../../hel/include/hel.h"

using namespace thor;

void readUserMemory(void *kern_ptr, const void *user_ptr, size_t size) {
	enableUserAccess();
	memcpy(kern_ptr, user_ptr, size);
	disableUserAccess();
}

void writeUserMemory(void *user_ptr, const void *kern_ptr, size_t size) {
	enableUserAccess();
	memcpy(user_ptr, kern_ptr, size);
	disableUserAccess();
}

template<typename T>
T readUserObject(const T *pointer) {
	T object;
	readUserMemory(&object, pointer, sizeof(T));
	return object;
}

template<typename T>
void writeUserObject(T *pointer, T object) {
	writeUserMemory(pointer, &object, sizeof(T));
}

template<typename T>
void readUserArray(const T *pointer, T *array, size_t count) {
	for(size_t i = 0; i < count; ++i)
		array[i] = readUserObject(pointer + i);
}

template<typename T>
void writeUserArray(T *pointer, const T *array, size_t count) {
	for(size_t i = 0; i < count; ++i)
		writeUserObject(pointer + i, array[i]);
}

size_t ipcSourceSize(size_t size) {
	return (size + 7) & ~size_t(7);
}

// TODO: one translate function per error source?
HelError translateError(Error error) {
	switch(error) {
	case kErrSuccess: return kHelErrNone;
	case kErrThreadExited: return kHelErrThreadTerminated;
	case kErrTransmissionMismatch: return kHelErrTransmissionMismatch;
	case kErrLaneShutdown: return kHelErrLaneShutdown;
	case kErrEndOfLane: return kHelErrEndOfLane;
	case kErrBufferTooSmall: return kHelErrBufferTooSmall;
	case kErrFault: return kHelErrFault;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}
}

template<typename P>
struct PostEvent {
public:
	struct Wrapper final : IpcNode {
		template<typename... Args>
		Wrapper(uintptr_t context, Args &&... args)
		: _writer{frigg::forward<Args>(args)...} {
			setupContext(context);
			setupSource(&_writer.source);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

	private:
		P _writer;
	};

	PostEvent(frigg::SharedPtr<IpcQueue> queue, uintptr_t context)
	: _queue(std::move(queue)), _context(context) { }

	template<typename... Args>
	void operator() (Args &&... args) {
		auto wrapper = frigg::construct<Wrapper>(*kernelAlloc,
				_context, frigg::forward<Args>(args)...);
		_queue->submit(wrapper);
	}

private:
	frigg::SharedPtr<IpcQueue> _queue;
	uintptr_t _context;
};

struct ObserveThreadWriter {
	ObserveThreadWriter(Error error, uint64_t sequence, Interrupt interrupt)
	: source{&_result, sizeof(HelObserveResult), nullptr},
			_result{translateError(error), 0, sequence} {
		if(interrupt == kIntrNull) {
			_result.observation = kHelObserveNull;
		}else if(interrupt == kIntrRequested) {
			_result.observation = kHelObserveInterrupt;
		}else if(interrupt == kIntrPanic) {
			_result.observation = kHelObservePanic;
		}else if(interrupt == kIntrBreakpoint) {
			_result.observation = kHelObserveBreakpoint;
		}else if(interrupt == kIntrPageFault) {
			_result.observation = kHelObservePageFault;
		}else if(interrupt == kIntrGeneralFault) {
			_result.observation = kHelObserveGeneralFault;
		}else if(interrupt == kIntrIllegalInstruction) {
			_result.observation = kHelObserveIllegalInstruction;
		}else if(interrupt >= kIntrSuperCall) {
			_result.observation = kHelObserveSuperCall + (interrupt - kIntrSuperCall);
		}else{
			frigg::panicLogger() << "Unexpected interrupt" << frigg::endLog;
			__builtin_unreachable();
		}
	}

	QueueSource source;

private:
	HelObserveResult _result;
};

HelError helLog(const char *string, size_t length) {
	size_t offset = 0;
	while(offset < length) {
		auto chunk = frigg::min(length - offset, size_t{100});

		char buffer[100];
		readUserArray(string + offset, buffer, chunk);
		{
			auto p = frigg::infoLogger();
			for(size_t i = 0; i < chunk; i++)
				p.print(buffer[i]);
		}
		offset += chunk;
	}

	return kHelErrNone;
}


HelError helCreateUniverse(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto new_universe = frigg::makeShared<Universe>(*kernelAlloc);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				UniverseDescriptor(std::move(new_universe)));
	}

	return kHelErrNone;
}

HelError helTransferDescriptor(HelHandle handle, HelHandle universe_handle,
		HelHandle *out_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	AnyDescriptor descriptor;
	frigg::SharedPtr<Universe> universe;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard lock(&this_universe->lock);

		auto descriptor_it = this_universe->getDescriptor(lock, handle);
		if(!descriptor_it)
			return kHelErrNoDescriptor;
		descriptor = *descriptor_it;

		if(universe_handle == kHelThisUniverse) {
			universe = this_universe.toShared();
		}else{
			auto universe_it = this_universe->getDescriptor(lock, universe_handle);
			if(!universe_it)
				return kHelErrNoDescriptor;
			if(!universe_it->is<UniverseDescriptor>())
				return kHelErrBadDescriptor;
			universe = universe_it->get<UniverseDescriptor>().universe;
		}
	}

	// TODO: make sure the descriptor is copyable.

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard lock(&universe->lock);

		*out_handle = universe->attachDescriptor(lock, std::move(descriptor));
	}
	return kHelErrNone;
}

HelError helDescriptorInfo(HelHandle handle, HelDescriptorInfo *info) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq_lock = frigg::guard(&irqMutex());
	Universe::Guard universe_guard(&this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	switch(wrapper->tag()) {
	default:
		assert(!"Illegal descriptor");
	}

	return kHelErrNone;
}

HelError helGetCredentials(HelHandle handle, uint32_t flags, char *credentials) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	assert(!flags);

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	writeUserMemory(credentials, thread->credentials(), 16);

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq_lock = frigg::guard(&irqMutex());
	Universe::Guard universe_guard(&this_universe->lock);

	if(!this_universe->detachDescriptor(universe_guard, handle))
		return kHelErrNoDescriptor;

	return kHelErrNone;
}

HelError helCreateQueue(HelQueue *head, uint32_t flags,
		unsigned int size_shift, size_t element_limit, HelHandle *handle) {
	assert(!flags);
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto queue = frigg::makeShared<IpcQueue>(*kernelAlloc,
			this_thread->getAddressSpace().lock(), head,
			size_shift, element_limit);
	queue->setupSelfPtr(queue);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				QueueDescriptor(std::move(queue)));
	}

	return kHelErrNone;
}

HelError helSetupChunk(HelHandle queue_handle, int index, HelChunk *chunk, uint32_t flags) {
	assert(!flags);
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	queue->setupChunk(index, this_thread->getAddressSpace().lock(), chunk);

	return kHelErrNone;
}

HelError helCancelAsync(HelHandle handle, uint64_t async_id) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	queue->cancel(async_id);

	return kHelErrNone;
}

HelError helAllocateMemory(size_t size, uint32_t flags,
		HelAllocRestrictions *restrictions, HelHandle *handle) {
	assert(size > 0);
	assert(size % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

//	auto pressure = physicalAllocator->numUsedPages() * kPageSize;
//	frigg::infoLogger() << "Allocate " << (void *)size
//			<< ", sum of allocated memory: " << (void *)pressure << frigg::endLog;

	HelAllocRestrictions effective{
		.addressBits = 64
	};
	if(restrictions)
		readUserMemory(&effective, restrictions, sizeof(HelAllocRestrictions));

	frigg::SharedPtr<MemoryView> memory;
	if(flags & kHelAllocContinuous) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits,
				size, kPageSize);
	}else if(flags & kHelAllocOnDemand) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits);
	}else{
		// TODO: 
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits);
	}

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helResizeMemory(HelHandle handle, size_t newSize) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryViewDescriptor>().memory;
	}

	Thread::asyncBlockCurrent([] (frigg::SharedPtr<MemoryView> memory, size_t newSize)
			-> coroutine<void> {
		co_await memory->resize(newSize);
	}(std::move(memory), newSize));

	return kHelErrNone;
}

HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle) {
	(void)flags;
	assert(!(size & (kPageSize - 1)));

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto managed = frigg::makeShared<ManagedSpace>(*kernelAlloc, size);
	auto backing_memory = frigg::makeShared<BackingMemory>(*kernelAlloc, managed);
	auto frontal_memory = frigg::makeShared<FrontalMemory>(*kernelAlloc, std::move(managed));

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*backing_handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(backing_memory)));
		*frontal_handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(frontal_memory)));
	}

	return kHelErrNone;
}

HelError helCopyOnWrite(HelHandle memoryHandle,
		uintptr_t offset, size_t size, HelHandle *outHandle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> view;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, memoryHandle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = wrapper->get<MemoryViewDescriptor>().memory;
	}

	auto slice = frigg::makeShared<CopyOnWriteMemory>(*kernelAlloc, std::move(view),
			offset, size);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*outHandle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(slice)));
	}

	return kHelErrNone;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	assert((physical % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<HardwareMemory>(*kernelAlloc, physical, size,
			CachingMode::null);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateIndirectMemory(size_t numSlots, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<IndirectMemory>(*kernelAlloc, numSlots);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helAlterMemoryIndirection(HelHandle indirectHandle, size_t slot,
		HelHandle memoryHandle, uintptr_t offset, size_t size) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	frigg::SharedPtr<MemoryView> indirectView;
	frigg::SharedPtr<MemoryView> memoryView;
	{
		auto irqLock = frigg::guard(&irqMutex());
		Universe::Guard universeLock(&thisUniverse->lock);

		auto indirectWrapper = thisUniverse->getDescriptor(universeLock, indirectHandle);
		if(!indirectWrapper)
			return kHelErrNoDescriptor;
		if(!indirectWrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		indirectView = indirectWrapper->get<MemoryViewDescriptor>().memory;

		auto memoryWrapper = thisUniverse->getDescriptor(universeLock, memoryHandle);
		if(!memoryWrapper)
			return kHelErrNoDescriptor;
		if(!memoryWrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memoryView = memoryWrapper->get<MemoryViewDescriptor>().memory;
	}

	if(auto e = indirectView->setIndirection(slot, std::move(memoryView), offset, size); e) {
		if(e == kErrIllegalObject) {
			return kHelErrUnsupportedOperation;
		}else{
			assert(e == kErrOutOfBounds);
			return kHelErrOutOfBounds;
		}
	}
	return kHelErrNone;
}

HelError helCreateSliceView(HelHandle memoryHandle,
		uintptr_t offset, size_t size, uint32_t flags, HelHandle *handle) {
	assert(!flags);
	assert((offset % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> view;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, memoryHandle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = wrapper->get<MemoryViewDescriptor>().memory;
	}

	auto slice = frigg::makeShared<MemorySlice>(*kernelAlloc,
			std::move(view), offset, size);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemorySliceDescriptor(std::move(slice)));
	}

	return kHelErrNone;
}

HelError helForkMemory(HelHandle handle, HelHandle *forkedHandle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> view;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto viewWrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!viewWrapper)
			return kHelErrNoDescriptor;
		if(!viewWrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = viewWrapper->get<MemoryViewDescriptor>().memory;
	}

	struct Closure {
		ThreadBlocker blocker;
		Error error;
		frigg::SharedPtr<MemoryView> forkedView;
	} closure;

	struct Receiver {
		void set_done(frg::tuple<Error, frigg::SharedPtr<MemoryView>> result) {
			closure->error = result.get<0>();
			closure->forkedView = std::move(result.get<1>());
			Thread::unblockOther(&closure->blocker);
		}

		Closure *closure;
	};

	closure.blocker.setup();
	view->fork(Receiver{&closure});
	Thread::blockCurrent(&closure.blocker);

	if(closure.error == kErrIllegalObject)
		return kHelErrUnsupportedOperation;
	assert(!closure.error);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*forkedHandle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(closure.forkedView));
	}

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto space = AddressSpace::create();

	auto irq_lock = frigg::guard(&irqMutex());
	Universe::Guard universe_guard(&this_universe->lock);

	*handle = this_universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(std::move(space)));

	return kHelErrNone;
}

HelError helCreateVirtualizedSpace(HelHandle *handle) {
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto irq_lock = frigg::guard(&irqMutex());
	auto this_universe = this_thread->getUniverse();

	PhysicalAddr pml4e = physicalAllocator->allocate(kPageSize);
	if(pml4e == static_cast<PhysicalAddr>(-1)) {
		return kHelErrNoMemory;
	}
	PageAccessor paccessor{pml4e};
	memset(paccessor.get(), 0, kPageSize);
	auto vspace = thor::vmx::EptSpace::create(pml4e);
	Universe::Guard universe_guard(&this_universe->lock);
	*handle = this_universe->attachDescriptor(universe_guard,
			VirtualizedSpaceDescriptor(std::move(vspace)));
	return kHelErrNone;
}

HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out) {
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto irq_lock = frigg::guard(&irqMutex());
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	Universe::Guard universe_guard(&this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!wrapper->is<VirtualizedSpaceDescriptor>())
		return kHelErrBadDescriptor;
	auto space = wrapper->get<VirtualizedSpaceDescriptor>();

	smarter::shared_ptr<vmx::Vmcs> vcpu = smarter::allocate_shared<vmx::Vmcs>(Allocator{}, (smarter::static_pointer_cast<thor::vmx::EptSpace>(space.space)));

	*out = this_universe->attachDescriptor(universe_guard,
			VirtualizedCpuDescriptor(std::move(vcpu)));
	return kHelErrNone;
}

HelError helRunVirtualizedCpu(HelHandle handle, HelVmexitReason *exitInfo) {
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	Universe::Guard universe_guard(&this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!wrapper->is<VirtualizedCpuDescriptor>())
		return kHelErrBadDescriptor;
	auto cpu = wrapper->get<VirtualizedCpuDescriptor>();
	auto info = cpu.vcpu->run();
	enableUserAccess();
	memcpy(exitInfo, &info, sizeof(HelVmexitReason));
	disableUserAccess();

	return kHelErrNone;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, uintptr_t offset, size_t length, uint32_t flags, void **actual_pointer) {
	if(length == 0)
		return kHelErrIllegalArgs;
	if((uintptr_t)pointer % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(offset % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(length % kPageSize != 0)
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	uint32_t map_flags = 0;
	if(pointer != nullptr) {
		map_flags |= AddressSpace::kMapFixed;
	}else{
		map_flags |= AddressSpace::kMapPreferTop;
	}

	if(flags & kHelMapProtRead)
		map_flags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		map_flags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		map_flags |= AddressSpace::kMapProtExecute;

	if(flags & kHelMapDontRequireBacking)
		map_flags |= AddressSpace::kMapDontRequireBacking;

	frigg::SharedPtr<MemorySlice> slice;
	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	smarter::shared_ptr<VirtualSpace> vspace;
	bool isVspace = false;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, memory_handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(memory_wrapper->is<MemorySliceDescriptor>()) {
			slice = memory_wrapper->get<MemorySliceDescriptor>().slice;
		}else if(memory_wrapper->is<MemoryViewDescriptor>()) {
			auto memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
			auto bundle_length = memory->getLength();
			slice = frigg::makeShared<MemorySlice>(*kernelAlloc,
					std::move(memory), 0, bundle_length);
		}else{
			return kHelErrBadDescriptor;
		}

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(space_wrapper->is<AddressSpaceDescriptor>()) {
				space = space_wrapper->get<AddressSpaceDescriptor>().space;
			} else if(space_wrapper->is<VirtualizedSpaceDescriptor>()) {
				isVspace = true;
				vspace = space_wrapper->get<VirtualizedSpaceDescriptor>().space;
			} else {
				return kHelErrBadDescriptor;
			}
		}
	}

	// TODO: check proper alignment

	VirtualAddr actual_address;
	Error error;
	if(!isVspace) {
		error = space->map(slice, (VirtualAddr)pointer, offset, length,
				map_flags, &actual_address);
	} else {
		error = vspace->map(slice, (VirtualAddr)pointer, offset, length,
						map_flags, &actual_address);
	}

	if(error == kErrBufferTooSmall) {
		return kHelErrBufferTooSmall;
	}else{
		assert(!error);
		*actual_pointer = (void *)actual_address;
		return kHelErrNone;
	}
}

HelError helSubmitProtectMemory(HelHandle space_handle,
		void *pointer, size_t length, uint32_t flags,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	uint32_t protectFlags = 0;
	if(flags & kHelMapProtRead)
		protectFlags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		protectFlags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		protectFlags |= AddressSpace::kMapProtExecute;

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	struct Closure final : IpcNode {
		Closure()
		: ipcSource{&helResult, sizeof(HelSimpleResult), nullptr} {
			setupSource(&ipcSource);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		frigg::SharedPtr<IpcQueue> ipcQueue;
		Worklet worklet;
		AddressProtectNode protect;
		QueueSource ipcSource;

		HelSimpleResult helResult;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void managed(Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);

			closure->helResult = HelSimpleResult{kHelErrNone};
			closure->ipcQueue->submit(closure);
		}
	};

	closure->ipcQueue = std::move(queue);
	closure->setupContext(context);

	closure->worklet.setup(&Ops::managed);
	closure->protect.setup(&closure->worklet);
	if(space->protect(reinterpret_cast<VirtualAddr>(pointer), length, protectFlags,
			&closure->protect)) {
		closure->helResult = HelSimpleResult{kHelErrNone};
		closure->ipcQueue->submit(closure);
		return kHelErrNone;
	}

	return kHelErrNone;
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	struct Closure {
		ThreadBlocker blocker;
		Worklet worklet;
		AddressUnmapNode node;
	} closure;

	closure.worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		Thread::unblockOther(&closure->blocker);
	});
	closure.node.setup(&closure.worklet);
	closure.blocker.setup();

	if(!space->unmap((VirtualAddr)pointer, length, &closure.node))
		Thread::blockCurrent(&closure.blocker);

	return kHelErrNone;
}

HelError helPointerPhysical(void *pointer, uintptr_t *physical) {
	auto this_thread = getCurrentThread();

	auto space = this_thread->getAddressSpace().lock();

	AcquireNode node;

	auto disp = (reinterpret_cast<uintptr_t>(pointer) & (kPageSize - 1));
	auto accessor = AddressSpaceLockHandle{std::move(space),
			reinterpret_cast<char *>(pointer) - disp, kPageSize};

	// FIXME: The physical page can change after we destruct the accessor!
	// We need a better hel API to properly handle that case.
	struct Closure {
		ThreadBlocker blocker;
		Worklet worklet;
		AcquireNode acquire;
	} closure;

	closure.worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		Thread::unblockOther(&closure->blocker);
	});
	closure.acquire.setup(&closure.worklet);
	closure.blocker.setup();
	if(!accessor.acquire(&closure.acquire))
		Thread::blockCurrent(&closure.blocker);

	auto page_physical = accessor.getPhysical(0);

	*physical = page_physical + disp;

	return kHelErrNone;
}

HelError helLoadForeign(HelHandle handle, uintptr_t address,
		size_t length, void *buffer) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(wrapper->is<AddressSpaceDescriptor>()) {
			space = wrapper->get<AddressSpaceDescriptor>().space;
		}else if(wrapper->is<ThreadDescriptor>()) {
			auto thread = wrapper->get<ThreadDescriptor>().thread;
			space = thread->getAddressSpace().lock();
		}else if(wrapper->is<VirtualizedSpaceDescriptor>()) {
			enableUserAccess();
			auto vspace = wrapper->get<VirtualizedSpaceDescriptor>().space;
			auto error = vspace->load(address, length, buffer);
			if(error == kErrFault) {
				disableUserAccess();
				return kHelErrFault;
			}
			return kHelErrNone;
		}else{
			return kHelErrBadDescriptor;
		}
	}

	auto accessor = AddressSpaceLockHandle{std::move(space),
			(void *)address, length};

	// TODO: Instead of blocking, this function should be asynchronous.
	struct Closure {
		ThreadBlocker blocker;
		Worklet worklet;
		AcquireNode acquire;
	} closure;

	closure.worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		Thread::unblockOther(&closure->blocker);
	});
	closure.acquire.setup(&closure.worklet);
	closure.blocker.setup();
	if(!accessor.acquire(&closure.acquire))
		Thread::blockCurrent(&closure.blocker);

	// TODO: This enableUserAccess() should be replaced by a writeUserMemory().
	enableUserAccess();
	accessor.load(0, buffer, length);
	disableUserAccess();

	return kHelErrNone;
}

HelError helStoreForeign(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(wrapper->is<AddressSpaceDescriptor>()) {
			space = wrapper->get<AddressSpaceDescriptor>().space;
		}else if(wrapper->is<ThreadDescriptor>()) {
			auto thread = wrapper->get<ThreadDescriptor>().thread;
			space = thread->getAddressSpace().lock();
		}else if(wrapper->is<VirtualizedSpaceDescriptor>()) {
			enableUserAccess();
			auto vspace = wrapper->get<VirtualizedSpaceDescriptor>().space;
			auto error = vspace->store(address, length, buffer);
			if(error == kErrFault) {
				disableUserAccess();
				return kHelErrFault;
			}
			return kHelErrNone;
		}else{
			return kHelErrBadDescriptor;
		}
	}

	auto accessor = AddressSpaceLockHandle{std::move(space),
			(void *)address, length};

	// TODO: Instead of blocking, this function should be asynchronous.
	struct Closure {
		ThreadBlocker blocker;
		Worklet worklet;
		AcquireNode acquire;
	} closure;

	closure.worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		Thread::unblockOther(&closure->blocker);
	});
	closure.acquire.setup(&closure.worklet);
	closure.blocker.setup();
	if(!accessor.acquire(&closure.acquire))
		Thread::blockCurrent(&closure.blocker);

	// TODO: This enableUserAccess() should be replaced by a readUserMemory().
	enableUserAccess();
	auto error = accessor.write(0, buffer, length);
	assert(!error);
	disableUserAccess();

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryViewDescriptor>().memory;
	}

	*size = memory->getLength();
	return kHelErrNone;
}

HelError helSubmitManageMemory(HelHandle handle, HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelManageResult))))
		return kHelErrQueueTooSmall;

	struct Closure final : IpcNode {
		Closure()
		: ipcSource{&helResult, sizeof(HelManageResult), nullptr} {
			setupSource(&ipcSource);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		frigg::SharedPtr<IpcQueue> ipcQueue;
		Worklet worklet;
		ManageNode manage;
		QueueSource ipcSource;

		HelManageResult helResult;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void managed(Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);

			int hel_type;
			switch(closure->manage.type()) {
			case ManageRequest::initialize: hel_type = kHelManageInitialize; break;
			case ManageRequest::writeback: hel_type = kHelManageWriteback; break;
			default:
				assert(!"unexpected ManageRequest");
				__builtin_trap();
			}

			closure->helResult = HelManageResult{translateError(closure->manage.error()),
					hel_type, closure->manage.offset(), closure->manage.size()};
			closure->ipcQueue->submit(closure);
		}
	};

	closure->ipcQueue = std::move(queue);
	closure->setupContext(context);

	closure->worklet.setup(&Ops::managed);
	closure->manage.setup(&closure->worklet);
	memory->submitManage(&closure->manage);

	return kHelErrNone;
}

HelError helUpdateMemory(HelHandle handle, int type,
		uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
	}

	Error error;
	switch(type) {
	case kHelManageInitialize:
		error = memory->updateRange(ManageRequest::initialize, offset, length);
		break;
	case kHelManageWriteback:
		error = memory->updateRange(ManageRequest::writeback, offset, length);
		break;
	default:
		return kHelErrIllegalArgs;
	}

	if(error == kErrIllegalObject)
		return kHelErrUnsupportedOperation;

	assert(!error);
	return kHelErrNone;
}

HelError helSubmitLockMemoryView(HelHandle handle, uintptr_t offset, size_t size,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelHandleResult))))
		return kHelErrQueueTooSmall;

	struct Closure final : IpcNode {
		Closure()
		: ipcSource{&helResult, sizeof(HelHandleResult), nullptr} {
			setupSource(&ipcSource);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		frigg::WeakPtr<Universe> weakUniverse;
		frigg::SharedPtr<IpcQueue> ipcQueue;
		Worklet worklet;
		frigg::SharedPtr<NamedMemoryViewLock> lock;
		MonitorNode initiate;
		QueueSource ipcSource;

		HelHandleResult helResult;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void initiated(Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);

			// Attach the descriptor.
			HelHandle handle;
			{
				auto universe = closure->weakUniverse.grab();
				assert(universe);

				auto irq_lock = frigg::guard(&irqMutex());
				Universe::Guard lock(&universe->lock);

				handle = universe->attachDescriptor(lock,
						MemoryViewLockDescriptor{std::move(closure->lock)});
			}

			closure->helResult = HelHandleResult{translateError(closure->initiate.error()),
					0, handle};
			closure->ipcQueue->submit(closure);
		}
	};

	closure->ipcQueue = std::move(queue);
	closure->setupContext(context);

	MemoryViewLockHandle lock_handle{memory, offset, size};
	if(!lock_handle) {
		// TODO: Return a better error.
		closure->helResult = HelHandleResult{kHelErrFault, 0, 0};
		closure->ipcQueue->submit(closure);
		return kHelErrNone;
	}

	closure->weakUniverse = this_universe.toWeak();
	closure->lock = frigg::makeShared<NamedMemoryViewLock>(*kernelAlloc, std::move(lock_handle));

	closure->worklet.setup(&Ops::initiated);
	closure->initiate.setup(ManageRequest::initialize, offset, size, &closure->worklet);
	memory->submitInitiateLoad(&closure->initiate);

	return kHelErrNone;
}

HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<MemoryView> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
	}

/*	auto handle_load = frigg::makeShared<AsyncInitiateLoad>(*kernelAlloc,
			NullCompleter(), offset, length);
	{
		// TODO: protect memory object with a guard
		memory->submitInitiateLoad(std::move(handle_load));
	}*/

	return kHelErrNone;
}

std::atomic<unsigned int> globalNextCpu = 0;

HelError helCreateThread(HelHandle universe_handle, HelHandle space_handle,
		int abi, void *ip, void *sp, uint32_t flags, HelHandle *handle) {
	(void)abi;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	if(flags & ~(kHelThreadStopped))
		return kHelErrIllegalArgs;

	frigg::SharedPtr<Universe> universe;
	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		if(universe_handle == kHelNullHandle) {
			universe = this_thread->getUniverse().toShared();
		}else{
			auto universe_wrapper = this_universe->getDescriptor(universe_guard, universe_handle);
			if(!universe_wrapper)
				return kHelErrNoDescriptor;
			if(!universe_wrapper->is<UniverseDescriptor>())
				return kHelErrBadDescriptor;
			universe = universe_wrapper->get<UniverseDescriptor>().universe;
		}

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().lock();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	AbiParameters params;
	params.ip = (uintptr_t)ip;
	params.sp = (uintptr_t)sp;

	auto new_thread = Thread::create(std::move(universe), std::move(space), params);
	new_thread->self = new_thread;

	// Adding a large prime (coprime to getCpuCount()) should yield a good distribution.
	auto cpu = globalNextCpu.fetch_add(4099, std::memory_order_relaxed) % getCpuCount();
//	frigg::infoLogger() << "thor: New thread on CPU #" << cpu << frigg::endLog;
	Scheduler::associate(new_thread.get(), &getCpuData(cpu)->scheduler);
//	Scheduler::associate(new_thread.get(), localScheduler());
	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(new_thread);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(std::move(new_thread)));
	}

	return kHelErrNone;
}

HelError helQueryThreadStats(HelHandle handle, HelThreadStats *user_stats) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	HelThreadStats stats;
	memset(&stats, 0, sizeof(HelThreadStats));
	stats.userTime = thread->runTime();

	writeUserObject(user_stats, stats);

	return kHelErrNone;
}

HelError helSetPriority(HelHandle handle, int priority) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	Scheduler::setPriority(thread.get(), priority);

	return kHelErrNone;
}

HelError helYield() {
	Thread::deferCurrent();

	return kHelErrNone;
}

HelError helSubmitObserve(HelHandle handle, uint64_t in_seq,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelObserveResult))))
		return kHelErrQueueTooSmall;

	PostEvent<ObserveThreadWriter> functor{std::move(queue), context};
	thread->submitObserve(in_seq, std::move(functor));

	return kHelErrNone;
}

HelError helKillThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	Thread::killOther(thread);

	return kHelErrNone;
}

HelError helInterruptThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	Thread::interruptOther(thread);

	return kHelErrNone;
}

HelError helResume(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	if(auto e = Thread::resumeOther(thread); e) {
		if(e == kErrThreadExited)
			return kHelErrThreadTerminated;
		assert(e == kErrIllegalState);
		return kHelErrIllegalState;
	}

	return kHelErrNone;
}

HelError helLoadRegisters(HelHandle handle, int set, void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	VirtualizedCpuDescriptor vcpu;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(thread_wrapper->is<ThreadDescriptor>()) {
			thread = thread_wrapper->get<ThreadDescriptor>().thread;
		} else if(thread_wrapper->is<VirtualizedCpuDescriptor>()) {
			vcpu = thread_wrapper->get<VirtualizedCpuDescriptor>();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	// TODO: Make sure that the thread is actually suspenend!

	if(set == kHelRegsProgram) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[2];
		regs[0] = *thread->_executor.ip();
		regs[1] = *thread->_executor.sp();
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2);
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[15];
		regs[0] = thread->_executor.general()->rax;
		regs[1] = thread->_executor.general()->rbx;
		regs[2] = thread->_executor.general()->rcx;
		regs[3] = thread->_executor.general()->rdx;
		regs[4] = thread->_executor.general()->rdi;
		regs[5] = thread->_executor.general()->rsi;
		regs[6] = thread->_executor.general()->r8;
		regs[7] = thread->_executor.general()->r9;
		regs[8] = thread->_executor.general()->r10;
		regs[9] = thread->_executor.general()->r11;
		regs[10] = thread->_executor.general()->r12;
		regs[11] = thread->_executor.general()->r13;
		regs[12] = thread->_executor.general()->r14;
		regs[13] = thread->_executor.general()->r15;
		regs[14] = thread->_executor.general()->rbp;
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 15);
	}else if(set == kHelRegsThread) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[2];
		regs[0] = thread->_executor.general()->clientFs;
		regs[1] = thread->_executor.general()->clientGs;
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2);
	}else if(set == kHelRegsVirtualization) {
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
		Universe::Guard universe_guard(&this_universe->lock);
		enableUserAccess();
		HelX86VirtualizationRegs* regs = (HelX86VirtualizationRegs*)image;
		vcpu.vcpu->loadRegs(regs);
		disableUserAccess();
		return kHelErrNone;
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	VirtualizedCpuDescriptor vcpu{0};
	if(handle == kHelThisThread) {
		// FIXME: Properly handle this below.
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(thread_wrapper->is<ThreadDescriptor>()) {
			thread = thread_wrapper->get<ThreadDescriptor>().thread;
		}else if(thread_wrapper->is<VirtualizedCpuDescriptor>()) {
			vcpu = thread_wrapper->get<VirtualizedCpuDescriptor>();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	// TODO: Make sure that the thread is actually suspenend!

	if(set == kHelRegsProgram) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[2];
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2);
		*thread->_executor.ip() = regs[0];
		*thread->_executor.sp() = regs[1];
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		uintptr_t regs[15];
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 15);
		thread->_executor.general()->rax = regs[0];
		thread->_executor.general()->rbx = regs[1];
		thread->_executor.general()->rcx = regs[2];
		thread->_executor.general()->rdx = regs[3];
		thread->_executor.general()->rdi = regs[4];
		thread->_executor.general()->rsi = regs[5];
		thread->_executor.general()->r8 = regs[6];
		thread->_executor.general()->r9 = regs[7];
		thread->_executor.general()->r10 = regs[8];
		thread->_executor.general()->r11 = regs[9];
		thread->_executor.general()->r12 = regs[10];
		thread->_executor.general()->r13 = regs[11];
		thread->_executor.general()->r14 = regs[12];
		thread->_executor.general()->r15 = regs[13];
		thread->_executor.general()->rbp = regs[14];
	}else if(set == kHelRegsThread) {
		uintptr_t regs[2];
		if(!thread) {
			return kHelErrIllegalArgs;
		}
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2);
		thread->_executor.general()->clientFs = regs[0];
		thread->_executor.general()->clientGs = regs[1];
	}else if(set == kHelRegsDebug) {
		// FIXME: Make those registers thread-specific.
		auto reg = readUserObject(reinterpret_cast<uint32_t *const *>(image));
		breakOnWrite(reg);
	}else if(set == kHelRegsVirtualization) {
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
		Universe::Guard universe_guard(&this_universe->lock);
		enableUserAccess();
		vcpu.vcpu->storeRegs((HelX86VirtualizationRegs*)image);
		disableUserAccess();
		return kHelErrNone;
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helWriteFsBase(void *pointer) {
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, (uintptr_t)pointer);
	return kHelErrNone;
}

HelError helGetClock(uint64_t *counter) {
	*counter = systemClockSource()->currentNanos();
	return kHelErrNone;
}

HelError helSubmitAwaitClock(uint64_t counter, HelHandle queue_handle, uintptr_t context,
		uint64_t *async_id) {
	struct Closure final : CancelNode, PrecisionTimerNode, IpcNode {
		static void issue(uint64_t nanos, frigg::SharedPtr<IpcQueue> queue,
				uintptr_t context, uint64_t *async_id) {
			auto closure = frigg::construct<Closure>(*kernelAlloc, nanos,
					std::move(queue), context);
			closure->queue->registerNode(closure);
			*async_id = closure->asyncId();
			generalTimerEngine()->installTimer(closure);
		}

		static void elapsed(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);
			if(closure->wasCancelled())
				closure->result.error = kHelErrCancelled;
			closure->queue->unregisterNode(closure);
			closure->queue->submit(closure);
		}

		explicit Closure(uint64_t nanos, frigg::SharedPtr<IpcQueue> the_queue,
				uintptr_t context)
		: queue{std::move(the_queue)},
				source{&result, sizeof(HelSimpleResult), nullptr},
				result{translateError(kErrSuccess), 0} {
			setupContext(context);
			setupSource(&source);

			worklet.setup(&Closure::elapsed);
			PrecisionTimerNode::setup(nanos, &worklet);
		}

		void handleCancellation() override {
			cancelTimer();
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		Worklet worklet;
		frigg::SharedPtr<IpcQueue> queue;
		QueueSource source;
		HelSimpleResult result;
	};

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	Closure::issue(counter, std::move(queue), context, async_id);

	return kHelErrNone;
}

HelError helCreateStream(HelHandle *lane1_handle, HelHandle *lane2_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto lanes = createStream();
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*lane1_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(std::move(lanes.get<0>())));
		*lane2_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(std::move(lanes.get<1>())));
	}

	return kHelErrNone;
}

HelError helSubmitAsync(HelHandle handle, const HelAction *actions, size_t count,
		HelHandle queue_handle, uintptr_t context, uint32_t flags) {
	(void)flags;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	// TODO: check userspace page access rights

	LaneHandle lane;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		if(handle == kHelThisThread) {
			lane = this_thread->inferiorLane();
		}else{
			auto wrapper = this_universe->getDescriptor(universe_guard, handle);
			if(!wrapper)
				return kHelErrNoDescriptor;
			if(wrapper->is<LaneDescriptor>()) {
				lane = wrapper->get<LaneDescriptor>().handle;
			}else if(wrapper->is<ThreadDescriptor>()) {
				lane = wrapper->get<ThreadDescriptor>().thread->superiorLane();
			}else{
				return kHelErrBadDescriptor;
			}
		}

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	size_t node_size = 0;
	for(size_t i = 0; i < count; i++) {
		HelAction action = readUserObject(actions + i);

		switch(action.type) {
		case kHelActionOffer:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionAccept:
			node_size += ipcSourceSize(sizeof(HelHandleResult));
			break;
		case kHelActionImbueCredentials:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionExtractCredentials:
			node_size += ipcSourceSize(sizeof(HelCredentialsResult));
			break;
		case kHelActionSendFromBuffer:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionSendFromBufferSg:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionRecvInline:
			// TODO: For now, we hardcode a size of 128 bytes.
			node_size += ipcSourceSize(sizeof(HelLengthResult));
			node_size += ipcSourceSize(128);
			break;
		case kHelActionRecvToBuffer:
			node_size += ipcSourceSize(sizeof(HelLengthResult));
			break;
		case kHelActionPushDescriptor:
			node_size += ipcSourceSize(sizeof(HelSimpleResult));
			break;
		case kHelActionPullDescriptor:
			node_size += ipcSourceSize(sizeof(HelHandleResult));
			break;
		default:
			// TODO: Turn this into an error return.
			assert(!"Fix error handling here");
		}
	}

	if(!queue->validSize(node_size))
		return kHelErrQueueTooSmall;

	struct Item {
		StreamNode transmit;
		frigg::UniqueMemory<KernelAlloc> buffer;
		QueueSource mainSource;
		QueueSource dataSource;
		union {
			HelSimpleResult helSimpleResult;
			HelHandleResult helHandleResult;
			HelCredentialsResult helCredentialsResult;
			HelInlineResultNoFlex helInlineResult;
			HelLengthResult helLengthResult;
		};
	};

	struct Closure final : IpcNode {
		void complete() override {
			// TODO: Turn items into a unique_ptr.
			frigg::destructN(*kernelAlloc, items, count);
			frigg::destruct(*kernelAlloc, this);
		}

		size_t count;
		frigg::WeakPtr<Universe> weakUniverse;
		frigg::SharedPtr<IpcQueue> ipcQueue;

		Worklet worklet;
		StreamPacket packet;
		Item *items;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void transmitted(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);

			QueueSource *tail = nullptr;
			auto link = [&] (QueueSource *source) {
				if(tail)
					tail->link = source;
				tail = source;
			};

			for(size_t i = 0; i < closure->count; i++) {
				auto item = &closure->items[i];
				if(item->transmit.tag() == kTagOffer) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagAccept) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(!item->transmit.error()) {
						auto universe = closure->weakUniverse.grab();
						assert(universe);

						auto irq_lock = frigg::guard(&irqMutex());
						Universe::Guard lock(&universe->lock);

						handle = universe->attachDescriptor(lock,
								LaneDescriptor{item->transmit.lane()});
					}

					item->helHandleResult = {translateError(item->transmit.error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagImbueCredentials) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagExtractCredentials) {
					item->helCredentialsResult = {translateError(item->transmit.error()), 0};
					memcpy(item->helCredentialsResult.credentials,
							item->transmit.credentials().data(), 16);
					item->mainSource.setup(&item->helCredentialsResult,
							sizeof(HelCredentialsResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagSendFromBuffer) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagRecvInline) {
					item->buffer = item->transmit.transmitBuffer();

					item->helInlineResult = {translateError(item->transmit.error()),
							0, item->buffer.size()};
					item->mainSource.setup(&item->helInlineResult, sizeof(HelInlineResultNoFlex));
					item->dataSource.setup(item->buffer.data(), item->buffer.size());
					link(&item->mainSource);
					link(&item->dataSource);
				}else if(item->transmit.tag() == kTagRecvToBuffer) {
					item->helLengthResult = {translateError(item->transmit.error()),
							0, item->transmit.actualLength()};
					item->mainSource.setup(&item->helLengthResult, sizeof(HelLengthResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagPushDescriptor) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagPullDescriptor) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(!item->transmit.error()) {
						auto universe = closure->weakUniverse.grab();
						assert(universe);

						auto irq_lock = frigg::guard(&irqMutex());
						Universe::Guard lock(&universe->lock);

						handle = universe->attachDescriptor(lock, item->transmit.descriptor());
					}

					item->helHandleResult = {translateError(item->transmit.error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else{
					frigg::panicLogger() << "thor: Unexpected transmission tag" << frigg::endLog;
				}
			}

			assert(closure->count);
			closure->setupSource(&closure->items[0].mainSource);
			closure->ipcQueue->submit(closure);
		}
	};

	closure->count = count;
	closure->weakUniverse = this_universe.toWeak();
	closure->ipcQueue = std::move(queue);

	closure->worklet.setup(&Ops::transmitted);
	closure->packet.setup(count, &closure->worklet);
	closure->setupContext(context);
	closure->items = frigg::constructN<Item>(*kernelAlloc, count);

	StreamList root_chain;
	frg::vector<StreamNode *, KernelAlloc> ancillary_stack(*kernelAlloc);

	// We use this as a marker that the root chain has not ended.
	ancillary_stack.push_back(nullptr);

	for(size_t i = 0; i < count; i++) {
		HelAction action = readUserObject(actions + i);

		// TODO: Turn this into an error return.
		assert(!ancillary_stack.empty() && "expected end of chain");

		switch(action.type) {
		case kHelActionOffer: {
			closure->items[i].transmit.setup(kTagOffer, &closure->packet);
		} break;
		case kHelActionAccept: {
			closure->items[i].transmit.setup(kTagAccept, &closure->packet);
		} break;
		case kHelActionImbueCredentials: {
			closure->items[i].transmit.setup(kTagImbueCredentials, &closure->packet);
			memcpy(closure->items[i].transmit._inCredentials.data(),
					this_thread->credentials(), 16);
		} break;
		case kHelActionExtractCredentials: {
			closure->items[i].transmit.setup(kTagExtractCredentials, &closure->packet);
		} break;
		case kHelActionSendFromBuffer: {
			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, action.length);
			readUserMemory(buffer.data(), action.buffer, action.length);

			closure->items[i].transmit.setup(kTagSendFromBuffer, &closure->packet);
			closure->items[i].transmit._inBuffer = std::move(buffer);
		} break;
		case kHelActionSendFromBufferSg: {
			size_t length = 0;
			auto sglist = reinterpret_cast<HelSgItem *>(action.buffer);
			for(size_t j = 0; j < action.length; j++) {
				auto item = readUserObject(sglist + j);
				length += item.length;
			}

			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, length);
			size_t offset = 0;
			for(size_t j = 0; j < action.length; j++) {
				auto item = readUserObject(sglist + j);
				readUserMemory(reinterpret_cast<char *>(buffer.data()) + offset,
						reinterpret_cast<char *>(item.buffer), item.length);
				offset += item.length;
			}

			closure->items[i].transmit.setup(kTagSendFromBuffer, &closure->packet);
			closure->items[i].transmit._inBuffer = std::move(buffer);
		} break;
		case kHelActionRecvInline: {
			// TODO: For now, we hardcode a size of 128 bytes.
			auto space = this_thread->getAddressSpace().lock();
			closure->items[i].transmit.setup(kTagRecvInline, &closure->packet);
			closure->items[i].transmit._maxLength = 128;
		} break;
		case kHelActionRecvToBuffer: {
			auto space = this_thread->getAddressSpace().lock();
			auto accessor = AddressSpaceLockHandle{std::move(space),
					action.buffer, action.length};

			// TODO: Instead of blocking here, the stream should acquire this asynchronously.
			struct AcqClosure {
				ThreadBlocker blocker;
				Worklet worklet;
				AcquireNode acquire;
			} acq_closure;

			acq_closure.worklet.setup([] (Worklet *base) {
				auto acq_closure = frg::container_of(base, &AcqClosure::worklet);
				Thread::unblockOther(&acq_closure->blocker);
			});
			acq_closure.acquire.setup(&acq_closure.worklet);
			acq_closure.blocker.setup();
			if(!accessor.acquire(&acq_closure.acquire))
				Thread::blockCurrent(&acq_closure.blocker);

			closure->items[i].transmit.setup(kTagRecvToBuffer, &closure->packet);
			closure->items[i].transmit._inAccessor = std::move(accessor);
		} break;
		case kHelActionPushDescriptor: {
			AnyDescriptor operand;
			{
				auto irq_lock = frigg::guard(&irqMutex());
				Universe::Guard universe_guard(&this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, action.handle);
				if(!wrapper)
					return kHelErrNoDescriptor;
				operand = *wrapper;
			}

			closure->items[i].transmit.setup(kTagPushDescriptor, &closure->packet);
			closure->items[i].transmit._inDescriptor = std::move(operand);
		} break;
		case kHelActionPullDescriptor: {
			closure->items[i].transmit.setup(kTagPullDescriptor, &closure->packet);
		} break;
		default:
			// TODO: Turn this into an error return.
			assert(!"Fix error handling here");
		}

		// Here, we make sure of our marker on the ancillary_stack.
		if(!ancillary_stack.back()) {
			// Add the item to the root list.
			root_chain.push_back(&closure->items[i].transmit);
		}else{
			// Add the item to an ancillary list.
			ancillary_stack.back()->ancillaryChain.push_back(&closure->items[i].transmit);
		}

		if(!(action.flags & kHelItemChain))
			ancillary_stack.pop();
		if(action.flags & kHelItemAncillary)
			ancillary_stack.push(&closure->items[i].transmit);
	}

	if(!ancillary_stack.empty())
		return kHelErrIllegalArgs;

	Stream::transmit(lane, root_chain);

	return kHelErrNone;
}

HelError helShutdownLane(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	LaneHandle lane;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<LaneDescriptor>())
			return kHelErrBadDescriptor;
		lane = wrapper->get<LaneDescriptor>().handle;
	}

	lane.getStream()->shutdownLane(lane.getLane());

	return kHelErrNone;
}

HelError helFutexWait(int *pointer, int expected) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	struct Closure final {
		ThreadBlocker blocker;
		Worklet worklet;
		FutexNode futex;
	} closure;

	// TODO: Support physical (i.e. non-private) futexes.
	closure.worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		Thread::unblockOther(&closure->blocker);
	});
	closure.futex.setup(&closure.worklet);
	closure.blocker.setup();
	space->futexSpace.submitWait(VirtualAddr(pointer), [&] () -> bool {
		enableUserAccess();
		auto v = __atomic_load_n(pointer, __ATOMIC_RELAXED);
		disableUserAccess();
		return expected == v;
	}, &closure.futex);

/*
	frigg::infoLogger() << "thor: "
			<< " " << this_thread->credentials()[0] << " " << this_thread->credentials()[1]
			<< " " << this_thread->credentials()[2] << " " << this_thread->credentials()[3]
			<< " " << this_thread->credentials()[4] << " " << this_thread->credentials()[5]
			<< " " << this_thread->credentials()[6] << " " << this_thread->credentials()[7]
			<< " " << this_thread->credentials()[8] << " " << this_thread->credentials()[9]
			<< " " << this_thread->credentials()[10] << " " << this_thread->credentials()[11]
			<< " " << this_thread->credentials()[12] << " " << this_thread->credentials()[13]
			<< " " << this_thread->credentials()[14] << " " << this_thread->credentials()[15]
			<< " Thread blocked on futex" << frigg::endLog;
*/

	Thread::blockCurrent(&closure.blocker);

	return kHelErrNone;
}

HelError helFutexWake(int *pointer) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	{
		// TODO: Support physical (i.e. non-private) futexes.
		space->futexSpace.wake(VirtualAddr(pointer));
	}

	return kHelErrNone;
}

HelError helCreateOneshotEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = frigg::makeShared<OneshotEvent>(*kernelAlloc);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				OneshotEventDescriptor(std::move(event)));
	}

	return kHelErrNone;
}

HelError helCreateBitsetEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = frigg::makeShared<BitsetEvent>(*kernelAlloc);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				BitsetEventDescriptor(std::move(event)));
	}

	return kHelErrNone;
}

HelError helRaiseEvent(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	AnyDescriptor descriptor;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;
	}

	if(descriptor.is<OneshotEventDescriptor>()) {
		auto event = descriptor.get<OneshotEventDescriptor>().event;
		event->trigger();
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helAccessIrq(int number, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq = frigg::makeShared<IrqObject>(*kernelAlloc,
			frigg::String<KernelAlloc>{*kernelAlloc, "generic-irq-object"});
	IrqPin::attachSink(getGlobalSystemIrq(number), irq.get());

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IrqDescriptor(std::move(irq)));
	}

	return kHelErrNone;
}

HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence) {
	assert(!(flags & ~(kHelAckAcknowledge | kHelAckNack | kHelAckKick)));

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto mode = flags & (kHelAckAcknowledge | kHelAckNack | kHelAckKick);
	if(mode != kHelAckAcknowledge && mode != kHelAckNack && mode != kHelAckKick)
		return kHelErrIllegalArgs;

	frigg::SharedPtr<IrqObject> irq;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;
	}

	Error error;
	if(mode == kHelAckAcknowledge) {
		error = IrqPin::ackSink(irq.get(), sequence);
	}else if(mode == kHelAckNack) {
		error = IrqPin::nackSink(irq.get(), sequence);
	}else{
 		assert(mode == kHelAckKick);
		error = IrqPin::kickSink(irq.get());
	}

	if(error == kErrIllegalArgs) {
		return kHelErrIllegalArgs;
	}else{
		assert(!error);
		return kHelErrNone;
	}
}

HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		HelHandle queue_handle, uintptr_t context) {
	struct IrqClosure final : IpcNode {
		static void issue(frigg::SharedPtr<IrqObject> irq, uint64_t sequence,
				frigg::SharedPtr<IpcQueue> queue, intptr_t context) {
			auto closure = frigg::construct<IrqClosure>(*kernelAlloc,
					std::move(queue), context);
			irq->submitAwait(&closure->irqNode, sequence);
		}

		static void awaited(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &IrqClosure::worklet);
			closure->result.error = translateError(closure->irqNode.error());
			closure->result.sequence = closure->irqNode.sequence();
			closure->_queue->submit(closure);
		}

	public:
		explicit IrqClosure(frigg::SharedPtr<IpcQueue> the_queue, uintptr_t context)
		: _queue{std::move(the_queue)},
				source{&result, sizeof(HelEventResult), nullptr} {
			memset(&result, 0, sizeof(HelEventResult));
			setupContext(context);
			setupSource(&source);
			worklet.setup(&IrqClosure::awaited);
			irqNode.setup(&worklet);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

	private:
		Worklet worklet;
		AwaitIrqNode irqNode;
		frigg::SharedPtr<IpcQueue> _queue;
		QueueSource source;
		HelEventResult result;
	};

	struct EventClosure final : IpcNode {
		static void issue(frigg::SharedPtr<OneshotEvent> event, uint64_t sequence,
				frigg::SharedPtr<IpcQueue> queue, intptr_t context) {
			auto closure = frigg::construct<EventClosure>(*kernelAlloc,
					std::move(queue), context);
			event->submitAwait(&closure->eventNode, sequence);
		}

		static void issue(frigg::SharedPtr<BitsetEvent> event, uint64_t sequence,
				frigg::SharedPtr<IpcQueue> queue, intptr_t context) {
			auto closure = frigg::construct<EventClosure>(*kernelAlloc,
					std::move(queue), context);
			event->submitAwait(&closure->eventNode, sequence);
		}

		static void awaited(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &EventClosure::worklet);
			closure->result.error = translateError(closure->eventNode.error());
			closure->result.sequence = closure->eventNode.sequence();
			closure->result.bitset = closure->eventNode.bitset();
			closure->_queue->submit(closure);
		}

	public:
		explicit EventClosure(frigg::SharedPtr<IpcQueue> the_queue, uintptr_t context)
		: _queue{std::move(the_queue)},
				source{&result, sizeof(HelEventResult), nullptr} {
			memset(&result, 0, sizeof(HelEventResult));
			setupContext(context);
			setupSource(&source);
			worklet.setup(&EventClosure::awaited);
			eventNode.setup(&worklet);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

	private:
		Worklet worklet;
		AwaitEventNode eventNode;
		frigg::SharedPtr<IpcQueue> _queue;
		QueueSource source;
		HelEventResult result;
	};

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqObject> irq;
	AnyDescriptor descriptor;
	frigg::SharedPtr<IpcQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelEventResult))))
		return kHelErrQueueTooSmall;

	if(descriptor.is<IrqDescriptor>()) {
		auto irq = descriptor.get<IrqDescriptor>().irq;
		IrqClosure::issue(std::move(irq), sequence,
				std::move(queue), context);
	}else if(descriptor.is<OneshotEventDescriptor>()) {
		auto event = descriptor.get<OneshotEventDescriptor>().event;
		EventClosure::issue(std::move(event), sequence,
				std::move(queue), context);
	}else if(descriptor.is<BitsetEventDescriptor>()) {
		auto event = descriptor.get<BitsetEventDescriptor>().event;
		EventClosure::issue(std::move(event), sequence,
				std::move(queue), context);
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helAutomateIrq(HelHandle handle, uint32_t flags, HelHandle kernlet_handle) {
	assert(!flags);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqObject> irq;
	frigg::SharedPtr<BoundKernlet> kernlet;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;

		auto kernlet_wrapper = this_universe->getDescriptor(universe_guard, kernlet_handle);
		if(!kernlet_wrapper)
			return kHelErrNoDescriptor;
		if(!kernlet_wrapper->is<BoundKernletDescriptor>())
			return kHelErrBadDescriptor;
		kernlet = kernlet_wrapper->get<BoundKernletDescriptor>().boundKernlet;
	}

	irq->automate(std::move(kernlet));

	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	// TODO: check userspace page access rights
	auto io_space = frigg::makeShared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++)
		io_space->addPort(readUserObject<uintptr_t>(port_array + i));

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IoDescriptor(std::move(io_space)));
	}

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IoSpace> io_space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<IoDescriptor>())
			return kHelErrBadDescriptor;
		io_space = wrapper->get<IoDescriptor>().ioSpace;
	}

	io_space->enableInThread(this_thread);

	return kHelErrNone;
}

HelError helEnableFullIo() {
	auto this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->getContext().enableIoPort(port);

	return kHelErrNone;
}

HelError helBindKernlet(HelHandle handle, const HelKernletData *data, size_t num_data,
		HelHandle *bound_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<KernletObject> kernlet;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto kernlet_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!kernlet_wrapper)
			return kHelErrNoDescriptor;
		if(!kernlet_wrapper->is<KernletObjectDescriptor>())
			return kHelErrBadDescriptor;
		kernlet = kernlet_wrapper->get<KernletObjectDescriptor>().kernletObject;
	}

	auto object = kernlet.get();
	assert(num_data == object->numberOfBindParameters());

	auto bound = frigg::makeShared<BoundKernlet>(*kernelAlloc,
			std::move(kernlet));
	for(size_t i = 0; i < object->numberOfBindParameters(); i++) {
		const auto &defn = object->defnOfBindParameter(i);

		enableUserAccess();
		auto x = data[i].handle;
		disableUserAccess();

		if(defn.type == KernletParameterType::offset) {
			bound->setupOffsetBinding(i, x);
		}else if(defn.type == KernletParameterType::memoryView) {
			frigg::SharedPtr<MemoryView> memory;
			{
				auto irq_lock = frigg::guard(&irqMutex());
				Universe::Guard universe_guard(&this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, x);
				if(!wrapper)
					return kHelErrNoDescriptor;
				if(!wrapper->is<MemoryViewDescriptor>())
					return kHelErrBadDescriptor;
				memory = wrapper->get<MemoryViewDescriptor>().memory;
			}

			auto window = reinterpret_cast<char *>(KernelVirtualMemory::global().allocate(0x10000));
			assert(memory->getLength() <= 0x10000);

			for(size_t off = 0; off < memory->getLength(); off += kPageSize) {
				auto range = memory->peekRange(off);
				assert(range.get<0>() != PhysicalAddr(-1));
				KernelPageSpace::global().mapSingle4k(reinterpret_cast<uintptr_t>(window + off),
						range.get<0>(), page_access::write, range.get<1>());
			}

			bound->setupMemoryViewBinding(i, window);
		}else{
			assert(defn.type == KernletParameterType::bitsetEvent);

			frigg::SharedPtr<BitsetEvent> event;
			{
				auto irq_lock = frigg::guard(&irqMutex());
				Universe::Guard universe_guard(&this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, x);
				if(!wrapper)
					return kHelErrNoDescriptor;
				if(!wrapper->is<BitsetEventDescriptor>())
					return kHelErrBadDescriptor;
				event = wrapper->get<BitsetEventDescriptor>().event;
			}

			bound->setupBitsetEventBinding(i, std::move(event));
		}
	}

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*bound_handle = this_universe->attachDescriptor(universe_guard,
				BoundKernletDescriptor(std::move(bound)));
	}

	return kHelErrNone;
}
