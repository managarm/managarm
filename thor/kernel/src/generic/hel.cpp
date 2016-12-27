
#include "kernel.hpp"

using namespace thor;

// TODO: one translate function per error source?
HelError translateError(Error error) {
	switch(error) {
	case kErrSuccess: return kHelErrNone;
//		case kErrClosedLocally: return kHelErrClosedLocally;
//		case kErrClosedRemotely: return kHelErrClosedRemotely;
//		case kErrBufferTooSmall: return kHelErrBufferTooSmall;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}
}

template<typename P>
struct PostEvent {
private:
	struct Functor {
		Functor(P writer)
		: _writer(frigg::move(writer)) { }

		void operator() (ForeignSpaceAccessor accessor) {
			_writer.write(frigg::move(accessor));
		}

	private:
		P _writer;
	};

public:
	PostEvent(frigg::SharedPtr<AddressSpace> space, void *queue, uintptr_t context)
	: _space(frigg::move(space)), _queue(queue), _context(context) {
		_handle = _space->queueSpace.prepare<Functor>();
	}
	
	template<typename... Args>
	void operator() (Args &&... args) {
		auto writer = P(frigg::forward<Args>(args)...);
		auto size = writer.size();
		_space->queueSpace.submit(frigg::move(_handle), _space, (uintptr_t)_queue,
				size, _context, Functor(frigg::move(writer)));
	}

private:
	frigg::SharedPtr<AddressSpace> _space;
	void *_queue;
	uintptr_t _context;
	QueueSpace::ElementHandle<Functor> _handle;
};

struct LockMemoryWriter {
	LockMemoryWriter(Error error)
	: _error(error) { }

	size_t size() {
		return sizeof(HelSimpleResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(0, &data, sizeof(HelSimpleResult));
	}

private:
	Error _error;
};

struct OfferWriter {
	OfferWriter(Error error)
	: _error(error) { }

	size_t size() {
		return sizeof(HelSimpleResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(0, &data, sizeof(HelSimpleResult));
	}

private:
	Error _error;
};

struct AcceptWriter {
	AcceptWriter(Error error, frigg::WeakPtr<Universe> universe, LaneDescriptor lane)
	: _error(error), _weakUniverse(frigg::move(universe)), _descriptor(frigg::move(lane)) { }

	size_t size() {
		return sizeof(HelHandleResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		Handle handle;
		{
			auto universe = _weakUniverse.grab();
			assert(universe);
			Universe::Guard lock(&universe->lock);
			handle = universe->attachDescriptor(lock, frigg::move(_descriptor));
		}

		HelHandleResult data{translateError(_error), 0, handle};
		accessor.copyTo(0, &data, sizeof(HelHandleResult));
	}

private:
	Error _error;
	frigg::WeakPtr<Universe> _weakUniverse;
	LaneDescriptor _descriptor;
};

struct SendStringWriter {
	SendStringWriter(Error error)
	: _error(error) { }

	size_t size() {
		return sizeof(HelSimpleResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(0, &data, sizeof(HelSimpleResult));
	}

private:
	Error _error;
};

struct RecvInlineWriter {
	RecvInlineWriter(Error error, frigg::UniqueMemory<KernelAlloc> buffer)
	: _error(error), _buffer(frigg::move(buffer)) { }

	size_t size() {
		size_t size = sizeof(HelInlineResult) + _buffer.size();
		return (size + 7) & ~size_t(7);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelInlineResult data{translateError(_error), 0, _buffer.size()};
		accessor.copyTo(0, &data, sizeof(HelInlineResult));
		accessor.copyTo(__builtin_offsetof(HelInlineResult, data),
				_buffer.data(), _buffer.size());
	}

private:
	Error _error;
	frigg::UniqueMemory<KernelAlloc> _buffer;
};

struct RecvStringWriter {
	RecvStringWriter(Error error, size_t length)
	: _error(error), _length(length) { }

	size_t size() {
		return sizeof(HelLengthResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelLengthResult data{translateError(_error), 0, _length};
		accessor.copyTo(0, &data, sizeof(HelLengthResult));
	}

private:
	Error _error;
	size_t _length;
};

struct PushDescriptorWriter {
	PushDescriptorWriter(Error error)
	: _error(error) { }

	size_t size() {
		return sizeof(HelSimpleResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(0, &data, sizeof(HelSimpleResult));
	}

private:
	Error _error;
};

struct PullDescriptorWriter {
	PullDescriptorWriter(Error error, frigg::WeakPtr<Universe> universe, AnyDescriptor descriptor)
	: _error(error), _weakUniverse(frigg::move(universe)), _lane(frigg::move(descriptor)) { }

	size_t size() {
		return sizeof(HelHandleResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		Handle handle;
		{
			auto universe = _weakUniverse.grab();
			assert(universe);
			Universe::Guard lock(&universe->lock);
			handle = universe->attachDescriptor(lock, frigg::move(_lane));
		}

		HelHandleResult data{translateError(_error), 0, handle};
		accessor.copyTo(0, &data, sizeof(HelHandleResult));
	}

private:
	Error _error;
	frigg::WeakPtr<Universe> _weakUniverse;
	AnyDescriptor _lane;
};

struct AwaitIrqWriter {
	AwaitIrqWriter(Error error)
	: _error(error) { }

	size_t size() {
		return sizeof(HelSimpleResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(0, &data, sizeof(HelSimpleResult));
	}

private:
	Error _error;
};

struct ObserveThreadWriter {
	ObserveThreadWriter(Error error, Interrupt interrupt)
	: _error(error), _interrupt(interrupt) { }

	size_t size() {
		return sizeof(HelObserveResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		unsigned int observation;
		if(_interrupt == kIntrBreakpoint) {
			observation = kHelObserveBreakpoint;
		}else if(_interrupt >= kIntrSuperCall) {
			observation = kHelObserveSuperCall + (_interrupt - kIntrSuperCall);
		}else{
			frigg::panicLogger() << "Unexpected interrupt" << frigg::endLog;
			__builtin_unreachable();
		}

		HelObserveResult data{translateError(_error), observation, 0};
		accessor.copyTo(0, &data, sizeof(HelSimpleResult));
	}

private:
	Error _error;
	Interrupt _interrupt;
};

HelError helLog(const char *string, size_t length) {
	for(size_t i = 0; i < length; i++)
		infoSink.print(string[i]);

	return kHelErrNone;
}


HelError helCreateUniverse(HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> this_universe = this_thread->getUniverse();

	auto new_universe = frigg::makeShared<Universe>(*kernelAlloc);
	
	{
		Universe::Guard universe_guard(&this_universe->lock);
		*handle = this_universe->attachDescriptor(universe_guard,
				UniverseDescriptor(frigg::move(new_universe)));
	}

	return kHelErrNone;
}

HelError helTransferDescriptor(HelHandle handle, HelHandle universe_handle,
		HelHandle *out_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> this_universe = this_thread->getUniverse();
	
	AnyDescriptor descriptor;
	frigg::SharedPtr<Universe> universe;
	{
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
		Universe::Guard lock(&universe->lock);
		*out_handle = universe->attachDescriptor(lock, frigg::move(descriptor));
	}
	return kHelErrNone;
}

HelError helDescriptorInfo(HelHandle handle, HelDescriptorInfo *user_info) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	auto wrapper = universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	switch(wrapper->tag()) {
	case AnyDescriptor::tagOf<EventHubDescriptor>():
		user_info->type = kHelDescEventHub;
		break;
	default:
		assert(!"Illegal descriptor");
	}
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	Universe::Guard universe_guard(&universe->lock);
	if(!universe->detachDescriptor(universe_guard, handle))
		return kHelErrNoDescriptor;
	universe_guard.unlock();

	return kHelErrNone;
}


HelError helAllocateMemory(size_t size, uint32_t flags, HelHandle *handle) {
	assert(size > 0);
	assert(size % kPageSize == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	frigg::SharedPtr<Memory> memory;
	if(flags & kHelAllocContinuous) {
		memory = frigg::makeShared<Memory>(*kernelAlloc,
				AllocatedMemory(size, size, kPageSize));
	}else if(flags & kHelAllocOnDemand) {
		memory = frigg::makeShared<Memory>(*kernelAlloc,
				AllocatedMemory(size));
	}else{
		// TODO: 
		memory = frigg::makeShared<Memory>(*kernelAlloc, AllocatedMemory(size));
	}
	
	{
		Universe::Guard universe_guard(&universe->lock);
		*handle = universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle) {
	(void)flags;
	assert(size > 0);
	assert(size % kPageSize == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	auto managed = frigg::makeShared<ManagedSpace>(*kernelAlloc, size);
	auto backing_memory = frigg::makeShared<Memory>(*kernelAlloc,
			BackingMemory(managed));
	auto frontal_memory = frigg::makeShared<Memory>(*kernelAlloc,
			FrontalMemory(frigg::move(managed)));
	
	{
		Universe::Guard universe_guard(&universe->lock);
		*backing_handle = universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(backing_memory)));
		*frontal_handle = universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(frontal_memory)));
	}

	return kHelErrNone;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	assert((physical % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<Memory>(*kernelAlloc, HardwareMemory(physical, size));
	{
		Universe::Guard universe_guard(&universe->lock);
		*handle = universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	space->setupDefaultMappings();
	
	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(frigg::move(space)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helForkSpace(HelHandle handle, HelHandle *forked_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		Universe::Guard universe_guard(&universe->lock);

		if(handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = universe->getDescriptor(universe_guard, handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	AddressSpace::Guard space_guard(&space->lock);
	auto forked = space->fork(space_guard);
	space_guard.unlock();
	
	{
		Universe::Guard universe_guard(&universe->lock);

		*forked_handle = universe->attachDescriptor(universe_guard,
				AddressSpaceDescriptor(frigg::move(forked)));
	}

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

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<AddressSpace> space;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, memory_handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
		
		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	// TODO: check proper alignment

	uint32_t map_flags = 0;
	if(pointer != nullptr) {
		map_flags |= AddressSpace::kMapFixed;
	}else{
		map_flags |= AddressSpace::kMapPreferTop;
	}

	constexpr int mask = kHelMapReadOnly | kHelMapReadWrite | kHelMapReadExecute;
	if((flags & mask) == kHelMapReadWrite) {
		map_flags |= AddressSpace::kMapReadWrite;
	}else if((flags & mask) == kHelMapReadExecute) {
		map_flags |= AddressSpace::kMapReadExecute;
	}else{
		assert((flags & mask) == kHelMapReadOnly);
		map_flags |= AddressSpace::kMapReadOnly;
	}

	if(flags & kHelMapDropAtFork) {
		map_flags |= AddressSpace::kMapDropAtFork;
	}else if(flags & kHelMapShareAtFork) {
		map_flags |= AddressSpace::kMapShareAtFork;
	}else if(flags & kHelMapCopyOnWriteAtFork) {
		map_flags |= AddressSpace::kMapCopyOnWriteAtFork;
	}

	if(flags & kHelMapDontRequireBacking)
		map_flags |= AddressSpace::kMapDontRequireBacking;
	
	VirtualAddr actual_address;
	AddressSpace::Guard space_guard(&space->lock);
	space->map(space_guard, memory, (VirtualAddr)pointer, offset, length,
			map_flags, &actual_address);
	space_guard.unlock();
	
	thorRtInvalidateSpace();

	*actual_pointer = (void *)actual_address;

	return kHelErrNone;
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&universe->lock);
	KernelSharedPtr<AddressSpace> space;
	if(space_handle == kHelNullHandle) {
		space = this_thread->getAddressSpace().toShared();
	}else{
		auto space_wrapper = universe->getDescriptor(universe_guard, space_handle);
		if(!space_wrapper)
			return kHelErrNoDescriptor;
		if(!space_wrapper->is<AddressSpaceDescriptor>())
			return kHelErrBadDescriptor;
		space = space_wrapper->get<AddressSpaceDescriptor>().space;
	}
	universe_guard.unlock();
	
	AddressSpace::Guard space_guard(&space->lock);
	space->unmap(space_guard, (VirtualAddr)pointer, length);
	space_guard.unlock();

	return kHelErrNone;
}

HelError helPointerPhysical(void *pointer, uintptr_t *physical) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	
	KernelSharedPtr<AddressSpace> space = this_thread->getAddressSpace().toShared();

	auto address = (VirtualAddr)pointer;
	auto misalign = address % kPageSize;

	PhysicalAddr page_physical;
	{
		AddressSpace::Guard space_guard(&space->lock);
		page_physical = space->grabPhysical(space_guard, address - misalign);
	}

	*physical = page_physical + misalign;

	return kHelErrNone;
}

HelError helLoadForeign(HelHandle handle, uintptr_t address,
		size_t length, void *buffer) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<AddressSpaceDescriptor>())
			return kHelErrBadDescriptor;
		space = wrapper->get<AddressSpaceDescriptor>().space;
	}
	
	auto accessor = ForeignSpaceAccessor::acquire(frigg::move(space),
			(void *)address, length);
	accessor.load(0, buffer, length);

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto wrapper = universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryAccessDescriptor>().memory;
	}

	*size = memory->getLength();
	return kHelErrNone;
}

HelError helSubmitProcessLoad(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
		
		auto hub_wrapper = universe->getDescriptor(universe_guard, hub_handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}
	
	PostEventCompleter completer(event_hub, allocAsyncId(), submit_function, submit_object);
	*async_id = completer.submitInfo.asyncId;
	
	auto initiate_load = frigg::makeShared<AsyncHandleLoad>(*kernelAlloc,
			frigg::move(completer));
	{
		// TODO: protect memory object with a guard
		memory->submitHandleLoad(frigg::move(initiate_load));
	}

	return kHelErrNone;
}

HelError helCompleteLoad(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
	}


	memory->completeLoad(offset, length);

	return kHelErrNone;
}

HelError helSubmitLockMemory(HelHandle handle, uintptr_t offset, size_t size,
		HelQueue *queue, uintptr_t context) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
	}

	PostEvent<LockMemoryWriter> functor{this_thread->getAddressSpace().toShared(), queue, context};
	auto initiate = frigg::makeShared<Initiate<PostEvent<LockMemoryWriter>>>(*kernelAlloc,
			offset, size, frigg::move(functor));
	{
		// TODO: protect memory object with a guard
		memory->submitInitiateLoad(frigg::move(initiate));
	}

	return kHelErrNone;
}

HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto memory_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
	}

/*	auto handle_load = frigg::makeShared<AsyncInitiateLoad>(*kernelAlloc,
			NullCompleter(), offset, length);
	{
		// TODO: protect memory object with a guard
		memory->submitInitiateLoad(frigg::move(handle_load));
	}*/
	
	return kHelErrNone;
}

HelError helCreateThread(HelHandle universe_handle, HelHandle space_handle,
		int abi, void *ip, void *sp, uint32_t flags, HelHandle *handle) {
	(void)abi;
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> this_universe = this_thread->getUniverse();

	if(flags & ~(kHelThreadExclusive | kHelThreadTrapsAreFatal
			| kHelThreadStopped))
		return kHelErrIllegalArgs;

	frigg::SharedPtr<Universe> universe;
	frigg::SharedPtr<AddressSpace> space;
	{
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
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	auto new_thread = frigg::makeShared<Thread>(*kernelAlloc, frigg::move(universe),
			frigg::move(space));
	if(flags & kHelThreadExclusive)
		new_thread->flags |= Thread::kFlagExclusive;
	if(flags & kHelThreadTrapsAreFatal)
		new_thread->flags |= Thread::kFlagTrapsAreFatal;
	
	new_thread->image.initSystemVAbi((Word)ip, (Word)sp, false);

	// we increment the owning refcount twice here.
	// it is decremented when all ThreadRunControl pointers go out of scope
	// AND when the thread is finally killed.
	new_thread.control().increment();
	new_thread.control().increment();
	frigg::SharedPtr<Thread, ThreadRunControl> run_ptr(frigg::adoptShared, new_thread.get(),
			ThreadRunControl(new_thread.get(), new_thread.control().counter()));

	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(new_thread);

	{
		Universe::Guard universe_guard(&this_universe->lock);
		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(frigg::move(run_ptr)));
	}

	return kHelErrNone;
}

HelError helYield() {
	assert(!intsAreEnabled());

	Thread::deferCurrent();

	return kHelErrNone;
}

HelError helSubmitObserve(HelHandle handle, HelQueue *queue, uintptr_t context) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Thread> thread;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto thread_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}
	
	// TODO: protect the thread with a lock!
	PostEvent<ObserveThreadWriter> functor{this_thread->getAddressSpace().toShared(), queue, context};
	thread->submitObserve(frigg::move(functor));

	return kHelErrNone;
}

HelError helResume(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto thread_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}	

	Thread::resumeOther(thread);

	return kHelErrNone;
}

HelError helLoadRegisters(HelHandle handle, int set, void *image) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto thread_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	if(set == kHelRegsProgram) {
		auto accessor = reinterpret_cast<uintptr_t *>(image);
		accessor[0] = *thread->image.ip();
		accessor[1] = *thread->image.sp();
	}else if(set == kHelRegsGeneral) {
		auto accessor = reinterpret_cast<uintptr_t *>(image);
		accessor[0] = thread->image.general()->rax;
		accessor[1] = thread->image.general()->rbx;
		accessor[2] = thread->image.general()->rcx;
		accessor[3] = thread->image.general()->rdx;
		accessor[4] = thread->image.general()->rdi;
		accessor[5] = thread->image.general()->rsi;
		accessor[6] = thread->image.general()->r8;
		accessor[7] = thread->image.general()->r9;
		accessor[8] = thread->image.general()->r10;
		accessor[9] = thread->image.general()->r11;
		accessor[10] = thread->image.general()->r12;
		accessor[11] = thread->image.general()->r13;
		accessor[12] = thread->image.general()->r14;
		accessor[13] = thread->image.general()->r15;
		accessor[14] = thread->image.general()->rbp;
	}else if(set == kHelRegsThread) {
		auto accessor = reinterpret_cast<uintptr_t *>(image);
		accessor[0] = thread->image.general()->clientFs;
		accessor[1] = thread->image.general()->clientGs;
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto thread_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}
	
	if(set == kHelRegsProgram) {
		auto accessor = reinterpret_cast<const uintptr_t *>(image);
		*thread->image.ip() = accessor[0];
		*thread->image.sp() = accessor[1];
	}else if(set == kHelRegsGeneral) {
		auto accessor = reinterpret_cast<const uintptr_t *>(image);
		thread->image.general()->rax = accessor[0];
		thread->image.general()->rbx = accessor[1];
		thread->image.general()->rcx = accessor[2];
		thread->image.general()->rdx = accessor[3];
		thread->image.general()->rdi = accessor[4];
		thread->image.general()->rsi = accessor[5];
		thread->image.general()->r8 = accessor[6];
		thread->image.general()->r9 = accessor[7];
		thread->image.general()->r10 = accessor[8];
		thread->image.general()->r11 = accessor[9];
		thread->image.general()->r12 = accessor[10];
		thread->image.general()->r13 = accessor[11];
		thread->image.general()->r14 = accessor[12];
		thread->image.general()->r15 = accessor[13];
		thread->image.general()->rbp = accessor[14];
	}else if(set == kHelRegsThread) {
		auto accessor = reinterpret_cast<const uintptr_t *>(image);
		thread->image.general()->clientFs = accessor[0];
		thread->image.general()->clientGs = accessor[1];
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helExitThisThread() {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();

	this_thread->signalKill();

	return kHelErrNone;
}

HelError helWriteFsBase(void *pointer) {
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, (uintptr_t)pointer);
	return kHelErrNone;
}

HelError helGetClock(uint64_t *counter) {
	*counter = currentNanos();
	return kHelErrNone;
}

HelError helCreateEventHub(HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto event_hub = frigg::makeShared<EventHub>(*kernelAlloc);

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			EventHubDescriptor(frigg::move(event_hub)));
	universe_guard.unlock();

	return kHelErrNone;
}

// TODO: move this to a private namespace?
static void translateToUserEvent(AsyncEvent event, HelEvent *user_event) {
	int type;
	switch(event.type) {
	case kEventMemoryLoad: type = kHelEventLoadMemory; break;
	case kEventMemoryLock: type = kHelEventLockMemory; break;
	case kEventObserve: type = kHelEventObserve; break;
	default:
		assert(!"Unexpected event type");
		__builtin_unreachable();
	}

	HelError error;
	switch(event.error) {
	case kErrSuccess: error = kHelErrNone; break;
	case kErrClosedLocally: error = kHelErrClosedLocally; break;
	case kErrClosedRemotely: error = kHelErrClosedRemotely; break;
	case kErrBufferTooSmall: error = kHelErrBufferTooSmall; break;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}

	auto accessor = DirectSelfAccessor<HelEvent>::acquire(user_event);
	accessor->type = type;
	accessor->error = error;
	accessor->asyncId = event.submitInfo.asyncId;
	accessor->submitFunction = event.submitInfo.submitFunction;
	accessor->submitObject = event.submitInfo.submitObject;

	accessor->msgRequest = event.msgRequest;
	accessor->msgSequence = event.msgSequence;
	accessor->offset = event.offset;
	accessor->length = event.length;
	accessor->handle = event.handle;
}

HelError helWaitForEvents(HelHandle handle,
		HelEvent *user_list, size_t max_items,
		HelNanotime max_nanotime, size_t *num_items) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto hub_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	assert(max_nanotime == kHelWaitInfinite);

	struct NullAllocator {
		void free(void *) { }
	};
	NullAllocator null_allocator;
		
	frigg::SharedBlock<AsyncWaitForEvent, NullAllocator> block(null_allocator,
			ReturnFromForkCompleter(this_thread.toWeak()), -1);
	frigg::SharedPtr<AsyncWaitForEvent> wait(frigg::adoptShared, &block);
	{
		EventHub::Guard hub_guard(&event_hub->lock);
		event_hub->submitWaitForEvent(hub_guard, wait);
	}

	Thread::blockCurrentWhile([&] {
		return !wait->isComplete.load(std::memory_order_acquire);
	});

	// TODO: support more than one event per transaction
	assert(max_items > 0);
	translateToUserEvent(wait->event, &user_list[0]);
	*num_items = 1;

	return kHelErrNone;
}

HelError helWaitForCertainEvent(HelHandle handle, int64_t async_id,
		HelEvent *user_event, HelNanotime max_nanotime) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<EventHub> event_hub;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto hub_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!hub_wrapper)
			return kHelErrNoDescriptor;
		if(!hub_wrapper->is<EventHubDescriptor>())
			return kHelErrBadDescriptor;
		event_hub = hub_wrapper->get<EventHubDescriptor>().eventHub;
	}

	assert(max_nanotime == kHelWaitInfinite);
	
	struct NullAllocator {
		void free(void *) { }
	};
	NullAllocator null_allocator;

	frigg::SharedBlock<AsyncWaitForEvent, NullAllocator> block(null_allocator,
			ReturnFromForkCompleter(this_thread.toWeak()), async_id);
	frigg::SharedPtr<AsyncWaitForEvent> wait(frigg::adoptShared, &block);
	{
		EventHub::Guard hub_guard(&event_hub->lock);
		event_hub->submitWaitForEvent(hub_guard, wait);
	}

	Thread::blockCurrentWhile([&] {
		return !wait->isComplete.load(std::memory_order_acquire);
	});

	assert(wait->event.submitInfo.asyncId == async_id);
	translateToUserEvent(wait->event, user_event);

	return kHelErrNone;
}


HelError helCreateStream(HelHandle *lane1_handle, HelHandle *lane2_handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto lanes = createStream();
	{
		Universe::Guard universe_guard(&universe->lock);
		*lane1_handle = universe->attachDescriptor(universe_guard,
				LaneDescriptor(frigg::move(lanes.get<0>())));
		*lane2_handle = universe->attachDescriptor(universe_guard,
				LaneDescriptor(frigg::move(lanes.get<1>())));
	}

	return kHelErrNone;
}

HelError helSubmitAsync(HelHandle handle, const HelAction *actions, size_t count,
		HelQueue *queue, uint32_t flags) {
	(void)flags;
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> this_universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	
	LaneHandle lane;
	if(handle == kHelThisThread) {
		lane = this_thread->inferiorLane();
	}else{
		Universe::Guard universe_guard(&this_universe->lock);

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

	frigg::Vector<LaneHandle, KernelAlloc> stack(*kernelAlloc);
	stack.push(frigg::move(lane));

	size_t i = 0;
	while(!stack.empty()) {
		assert(i < count);
		HelAction action = actions[i++];

		auto target = stack.back();
		if(!(action.flags & kHelItemChain))
			stack.pop();

		switch(action.type) {
		case kHelActionOffer: {
			using Token = PostEvent<OfferWriter>;
			LaneHandle branch = target.getStream()->submitOffer(target.getLane(),
					Token(this_thread->getAddressSpace().toShared(), queue, action.context));

			if(action.flags & kHelItemAncillary)
				stack.push(branch);
		} break;
		case kHelActionAccept: {
			using Token = PostEvent<AcceptWriter>;
			LaneHandle branch = target.getStream()->submitAccept(target.getLane(),
					this_universe.toWeak(),
					Token(this_thread->getAddressSpace().toShared(), queue, action.context));

			if(action.flags & kHelItemAncillary)
				stack.push(branch);
		} break;
		case kHelActionSendFromBuffer: {
			using Token = PostEvent<SendStringWriter>;
			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, action.length);
			memcpy(buffer.data(), action.buffer, action.length);
			target.getStream()->submitSendBuffer(target.getLane(), frigg::move(buffer),
					Token(this_thread->getAddressSpace().toShared(), queue, action.context));
		} break;
		case kHelActionRecvInline: {
			using Token = PostEvent<RecvInlineWriter>;
			auto space = this_thread->getAddressSpace().toShared();
			target.getStream()->submitRecvInline(target.getLane(),
					Token(this_thread->getAddressSpace().toShared(), queue, action.context));
		} break;
		case kHelActionRecvToBuffer: {
			using Token = PostEvent<RecvStringWriter>;
			auto space = this_thread->getAddressSpace().toShared();
			auto accessor = ForeignSpaceAccessor::acquire(frigg::move(space),
					action.buffer, action.length);
			target.getStream()->submitRecvBuffer(target.getLane(), frigg::move(accessor),
					Token(this_thread->getAddressSpace().toShared(), queue, action.context));
		} break;
		case kHelActionPushDescriptor: {
			AnyDescriptor operand;
			{
				Universe::Guard universe_guard(&this_universe->lock);
				auto wrapper = this_universe->getDescriptor(universe_guard, action.handle);
				if(!wrapper)
					return kHelErrNoDescriptor;
				operand = *wrapper;
			}

			using Token = PostEvent<PushDescriptorWriter>;
			target.getStream()->submitPushDescriptor(target.getLane(), frigg::move(operand),
					Token(this_thread->getAddressSpace().toShared(), queue, action.context));
		} break;
		case kHelActionPullDescriptor: {
			using Token = PostEvent<PullDescriptorWriter>;
			target.getStream()->submitPullDescriptor(target.getLane(), this_universe.toWeak(),
					Token(this_thread->getAddressSpace().toShared(), queue, action.context));
		} break;
		default:
			assert(!"Fix error handling here");
		}
	}
	assert(i == count);

	return kHelErrNone;
}


HelError helFutexWait(int *pointer, int expected) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	std::atomic<bool> complete(false);
	{
		// FIXME: the mapping needs to be protected after the lock on the AddressSpace is released.
		Mapping *mapping;
		{
			AddressSpace::Guard space_guard(&space->lock);
			mapping = space->getMapping(VirtualAddr(pointer));
		}
		assert(mapping->type == Mapping::kTypeMemory);

		auto futex = &mapping->memoryRegion->futex;
		futex->waitIf(VirtualAddr(pointer) - mapping->baseAddress, [&] () -> bool {
			auto v = __atomic_load_n(pointer, __ATOMIC_RELAXED);
			return expected == v;
		}, [&] {
			complete.store(true, std::memory_order_release);
			Thread::unblockOther(this_thread);
		});
	}

	Thread::blockCurrentWhile([&] {
		return !complete.load(std::memory_order_acquire);
	});

	return kHelErrNone;
}

HelError helFutexWake(int *pointer) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	{
		// FIXME: the mapping needs to be protected after the lock on the AddressSpace is released.
		Mapping *mapping;
		{
			AddressSpace::Guard space_guard(&space->lock);
			mapping = space->getMapping(VirtualAddr(pointer));
		}
		assert(mapping->type == Mapping::kTypeMemory);

		auto futex = &mapping->memoryRegion->futex;
		futex->wake(VirtualAddr(pointer) - mapping->baseAddress);
	}

	return kHelErrNone;
}


HelError helAccessIrq(int number, HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	auto irq_line = frigg::makeShared<IrqLine>(*kernelAlloc, number);
	
	IrqRelay::Guard irq_guard(&irqRelays[number]->lock);
	irqRelays[number]->addLine(irq_guard, frigg::WeakPtr<IrqLine>(irq_line));
	irq_guard.unlock();

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			IrqDescriptor(frigg::move(irq_line)));
	universe_guard.unlock();

	return kHelErrNone;
}
HelError helSetupIrq(HelHandle handle, uint32_t flags) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IrqLine> irq_line;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq_line = irq_wrapper->get<IrqDescriptor>().irqLine;
	}

	uint32_t relay_flags = 0;
	if(flags & kHelIrqManualAcknowledge)
		relay_flags |= IrqRelay::kFlagManualAcknowledge;
	
	int number = irq_line->getNumber();

	IrqRelay::Guard relay_guard(&irqRelays[number]->lock);
	irqRelays[number]->setup(relay_guard, relay_flags);
	relay_guard.unlock();

	return kHelErrNone;
}
HelError helAcknowledgeIrq(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IrqLine> irq_line;
	{
		Universe::Guard universe_guard(&universe->lock);
		auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq_line = irq_wrapper->get<IrqDescriptor>().irqLine;
	}

	int number = irq_line->getNumber();
	
	IrqRelay::Guard relay_guard(&irqRelays[number]->lock);
	irqRelays[number]->manualAcknowledge(relay_guard);
	relay_guard.unlock();

	return kHelErrNone;
}
HelError helSubmitWaitForIrq(HelHandle handle,
		HelQueue *queue, uintptr_t context) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqLine> line;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto irq_wrapper = universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		line = irq_wrapper->get<IrqDescriptor>().irqLine;
	}

	PostEvent<AwaitIrqWriter> functor{this_thread->getAddressSpace().toShared(), queue, context};
	auto wait = frigg::makeShared<AwaitIrq<PostEvent<AwaitIrqWriter>>>(*kernelAlloc,
			frigg::move(functor));
	{
		IrqLine::Guard guard(&line->lock);
		line->submitWait(guard, frigg::move(wait));
	}
	
	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *user_port_array, size_t num_ports,
		HelHandle *handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	auto io_space = frigg::makeShared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++)
		io_space->addPort(user_port_array[i]);

	Universe::Guard universe_guard(&universe->lock);
	*handle = universe->attachDescriptor(universe_guard,
			IoDescriptor(frigg::move(io_space)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<Universe> universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IoSpace> io_space;
	{
		Universe::Guard universe_guard(&universe->lock);

		auto wrapper = universe->getDescriptor(universe_guard, handle);
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
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->getContext().enableIoPort(port);

	return kHelErrNone;
}

