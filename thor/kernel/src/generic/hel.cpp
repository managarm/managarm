
#include "kernel.hpp"
#include "irq.hpp"

using namespace thor;

// TODO: one translate function per error source?
HelError translateError(Error error) {
	switch(error) {
	case kErrSuccess: return kHelErrNone;
//		case kErrClosedLocally: return kHelErrClosedLocally;
	case kErrClosedRemotely: return kHelErrClosedRemotely;
//		case kErrBufferTooSmall: return kHelErrBufferTooSmall;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}
}

template<typename P>
struct PostEvent : QueueNode {
public:
	PostEvent(frigg::SharedPtr<AddressSpace> space, void *queue, uintptr_t context)
	: _space(frigg::move(space)), _queue(queue) {
		setupContext(context);
	}
	
	template<typename... Args>
	void operator() (Args &&... args) {
		_writer = frigg::construct<P>(*kernelAlloc, frigg::forward<Args>(args)...);
		setupLength(_writer->size());
		_space->queueSpace.submit(_space, (uintptr_t)_queue, this);
	}

	void emit(ForeignSpaceAccessor accessor) override {
		_writer->write(frigg::move(accessor));
		frigg::destruct(*kernelAlloc, _writer);
	}

private:
	frigg::SharedPtr<AddressSpace> _space;
	void *_queue;
	P *_writer;
};

struct ManageMemoryWriter {
	ManageMemoryWriter(Error error, uintptr_t offset, size_t length)
	: _error(error), _offset(offset), _length(length) { }

	size_t size() {
		return sizeof(HelManageResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		HelManageResult data{translateError(_error), 0, _offset, _length};
		accessor.copyTo(0, &data, sizeof(HelManageResult));
	}

private:
	Error _error;
	uintptr_t _offset;
	size_t _length;
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

	void write(ForeignSpaceAccessor &accessor, uintptr_t disp) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(disp, &data, sizeof(HelSimpleResult));
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

	void write(ForeignSpaceAccessor &accessor, uintptr_t disp) {
		Handle handle = kHelNullHandle;
		if(_weakUniverse) {
			auto universe = _weakUniverse.grab();
			assert(universe);
			Universe::Guard lock(&universe->lock);
			handle = universe->attachDescriptor(lock, frigg::move(_descriptor));
		}

		HelHandleResult data{translateError(_error), 0, handle};
		accessor.copyTo(disp, &data, sizeof(HelHandleResult));
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

	void write(ForeignSpaceAccessor &accessor, uintptr_t disp) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(disp, &data, sizeof(HelSimpleResult));
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

	void write(ForeignSpaceAccessor &accessor, uintptr_t disp) {
		if(_buffer) {
			HelInlineResult data{translateError(_error), 0, _buffer.size()};
			accessor.copyTo(disp, &data, sizeof(HelInlineResult));
			accessor.copyTo(disp + __builtin_offsetof(HelInlineResult, data),
					_buffer.data(), _buffer.size());
		}else{
			HelInlineResult data{translateError(_error), 0, 0};
			accessor.copyTo(disp, &data, sizeof(HelInlineResult));
		}
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

	void write(ForeignSpaceAccessor &accessor, uintptr_t disp) {
		HelLengthResult data{translateError(_error), 0, _length};
		accessor.copyTo(disp, &data, sizeof(HelLengthResult));
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

	void write(ForeignSpaceAccessor &accessor, uintptr_t disp) {
		HelSimpleResult data{translateError(_error), 0};
		accessor.copyTo(disp, &data, sizeof(HelSimpleResult));
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

	void write(ForeignSpaceAccessor &accessor, uintptr_t disp) {
		Handle handle = kHelNullHandle;
		if(_weakUniverse) {
			auto universe = _weakUniverse.grab();
			assert(universe);
			Universe::Guard lock(&universe->lock);
			handle = universe->attachDescriptor(lock, frigg::move(_lane));
		}

		HelHandleResult data{translateError(_error), 0, handle};
		accessor.copyTo(disp, &data, sizeof(HelHandleResult));
	}

private:
	Error _error;
	frigg::WeakPtr<Universe> _weakUniverse;
	AnyDescriptor _lane;
};

struct ObserveThreadWriter {
	ObserveThreadWriter(Error error, Interrupt interrupt)
	: _error(error), _interrupt(interrupt) { }

	size_t size() {
		return sizeof(HelObserveResult);
	}

	void write(ForeignSpaceAccessor accessor) {
		unsigned int observation;
		if(_interrupt == kIntrStop) {
			observation = kHelObserveStop;
		}else if(_interrupt == kIntrPanic) {
			observation = kHelObservePanic;
		}else if(_interrupt == kIntrBreakpoint) {
			observation = kHelObserveBreakpoint;
		}else if(_interrupt == kIntrPageFault) {
			observation = kHelObservePageFault;
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

using ItemWriter = frigg::Variant<
	OfferWriter,
	AcceptWriter,
	SendStringWriter,
	RecvInlineWriter,
	RecvStringWriter,
	PushDescriptorWriter,
	PullDescriptorWriter
>;

struct MsgHandler : QueueNode {
	template<typename W>
	friend struct SetResult;

public:
	MsgHandler(size_t num_items, frigg::SharedPtr<AddressSpace> space,
			void *queue, uintptr_t context)
	: _results(*kernelAlloc), _numComplete(0),
			_space(frigg::move(space)), _queue(queue) {
		setupContext(context);
		_results.resize(num_items);
	}

private:
	void complete() {
		size_t size = 0;
		for(size_t i = 0; i < _results.size(); ++i) {
			size += _results[i].apply([] (auto &writer) -> size_t {
				return writer.size();
			});
		}

		setupLength(size);
		_space->queueSpace.submit(_space, (uintptr_t)_queue, this);
	}
	
	void emit(ForeignSpaceAccessor accessor) override {
		size_t disp = 0;
		for(size_t i = 0; i < _results.size(); ++i) {
			_results[i].apply([&] (auto &writer) {
				assert(!(disp & 7)); // TODO: Replace the magic constant by alignof(...).
				writer.write(accessor, disp);
				disp += writer.size();
			});
		}
	}

	frigg::Vector<ItemWriter, KernelAlloc> _results;
	std::atomic<unsigned int> _numComplete;

	frigg::SharedPtr<AddressSpace> _space;
	void *_queue;
};

template<typename W>
struct SetResult {
	SetResult(MsgHandler *handler, size_t index)
	: _handler(handler), _index(index) { }

	template<typename... Args>
	void operator() (Args &&... args) {
		_handler->_results[_index] = W{frigg::forward<Args>(args)...};
		auto c = _handler->_numComplete.fetch_add(1, std::memory_order_acq_rel);
		if(c + 1 == _handler->_results.size())
			_handler->complete();
	}

private:
	MsgHandler *_handler;
	size_t _index;
};

HelError helLog(const char *string, size_t length) {
	for(size_t i = 0; i < length; i++)
		infoSink.print(string[i]);

	return kHelErrNone;
}


HelError helCreateUniverse(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&this_universe->lock);
	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	switch(wrapper->tag()) {
	default:
		assert(!"Illegal descriptor");
	}
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	Universe::Guard universe_guard(&this_universe->lock);
	if(!this_universe->detachDescriptor(universe_guard, handle))
		return kHelErrNoDescriptor;
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helAllocateMemory(size_t size, uint32_t flags, HelHandle *handle) {
	assert(size > 0);
	assert(size % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

//	auto pressure = physicalAllocator->numUsedPages() * kPageSize;
//	frigg::infoLogger() << "Allocate " << (void *)size
//			<< ", sum of allocated memory: " << (void *)pressure << frigg::endLog;

	frigg::SharedPtr<Memory> memory;
	if(flags & kHelAllocContinuous) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, size, kPageSize);
	}else if(flags & kHelAllocOnDemand) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size);
	}else{
		// TODO: 
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size);
	}
	
	{
		Universe::Guard universe_guard(&this_universe->lock);
		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle) {
	(void)flags;
	assert(size > 0);
	assert(size % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto managed = frigg::makeShared<ManagedSpace>(*kernelAlloc, size);
	auto backing_memory = frigg::makeShared<BackingMemory>(*kernelAlloc, managed);
	auto frontal_memory = frigg::makeShared<FrontalMemory>(*kernelAlloc, frigg::move(managed));
	
	{
		Universe::Guard universe_guard(&this_universe->lock);
		*backing_handle = this_universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(backing_memory)));
		*frontal_handle = this_universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(frontal_memory)));
	}

	return kHelErrNone;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	assert((physical % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<HardwareMemory>(*kernelAlloc, physical, size);
	{
		Universe::Guard universe_guard(&this_universe->lock);
		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc);
	space->setupDefaultMappings();
	
	Universe::Guard universe_guard(&this_universe->lock);
	*handle = this_universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(frigg::move(space)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helForkSpace(HelHandle handle, HelHandle *forked_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		if(handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, handle);
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
		Universe::Guard universe_guard(&this_universe->lock);

		*forked_handle = this_universe->attachDescriptor(universe_guard,
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

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<AddressSpace> space;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, memory_handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
		
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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	Universe::Guard universe_guard(&this_universe->lock);
	frigg::SharedPtr<AddressSpace> space;
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
	universe_guard.unlock();
	
	AddressSpace::Guard space_guard(&space->lock);
	space->unmap(space_guard, (VirtualAddr)pointer, length);
	space_guard.unlock();

	return kHelErrNone;
}

HelError helPointerPhysical(void *pointer, uintptr_t *physical) {
	auto this_thread = getCurrentThread();
	
	frigg::SharedPtr<AddressSpace> space = this_thread->getAddressSpace().toShared();

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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryAccessDescriptor>().memory;
	}

	*size = memory->getLength();
	return kHelErrNone;
}

HelError helSubmitManageMemory(HelHandle handle, HelQueue *queue, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&this_universe->lock);
		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryAccessDescriptor>().memory;
	}
	
	PostEvent<ManageMemoryWriter> functor{this_thread->getAddressSpace().toShared(),
			queue, context};
	auto manage = frigg::makeShared<Manage<PostEvent<ManageMemoryWriter>>>(*kernelAlloc,
			frigg::move(functor));
	{
		// TODO: protect memory object with a guard
		memory->submitHandleLoad(frigg::move(manage));
	}

	return kHelErrNone;
}

HelError helCompleteLoad(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
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

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

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
	
	AbiParameters params;
	params.ip = (uintptr_t)ip;
	params.sp = (uintptr_t)sp;

	auto new_thread = Thread::create(frigg::move(universe), frigg::move(space), params);
	new_thread->self = new_thread;
	if(flags & kHelThreadExclusive)
		new_thread->flags |= Thread::kFlagExclusive;
	if(flags & kHelThreadTrapsAreFatal)
		new_thread->flags |= Thread::kFlagTrapsAreFatal;

	globalScheduler().attach(new_thread.get());
	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(new_thread);

	{
		Universe::Guard universe_guard(&this_universe->lock);
		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(frigg::move(new_thread)));
	}

	return kHelErrNone;
}

HelError helYield() {
	assert(!intsAreEnabled());

	Thread::deferCurrent();

	return kHelErrNone;
}

HelError helSubmitObserve(HelHandle handle, HelQueue *queue, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Thread> thread;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
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
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	if(set == kHelRegsProgram) {
		auto accessor = reinterpret_cast<uintptr_t *>(image);
		accessor[0] = *thread->_executor.ip();
		accessor[1] = *thread->_executor.sp();
	}else if(set == kHelRegsGeneral) {
		auto accessor = reinterpret_cast<uintptr_t *>(image);
		accessor[0] = thread->_executor.general()->rax;
		accessor[1] = thread->_executor.general()->rbx;
		accessor[2] = thread->_executor.general()->rcx;
		accessor[3] = thread->_executor.general()->rdx;
		accessor[4] = thread->_executor.general()->rdi;
		accessor[5] = thread->_executor.general()->rsi;
		accessor[6] = thread->_executor.general()->r8;
		accessor[7] = thread->_executor.general()->r9;
		accessor[8] = thread->_executor.general()->r10;
		accessor[9] = thread->_executor.general()->r11;
		accessor[10] = thread->_executor.general()->r12;
		accessor[11] = thread->_executor.general()->r13;
		accessor[12] = thread->_executor.general()->r14;
		accessor[13] = thread->_executor.general()->r15;
		accessor[14] = thread->_executor.general()->rbp;
	}else if(set == kHelRegsThread) {
		auto accessor = reinterpret_cast<uintptr_t *>(image);
		accessor[0] = thread->_executor.general()->clientFs;
		accessor[1] = thread->_executor.general()->clientGs;
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

#include "../arch/x86/debug.hpp"

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		// FIXME: Properly handle this below.
		thread = this_thread.toShared();
	}else{
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}
	
	// FIXME: We need to lock the thread and ensure it is in the interrupted state.
	if(set == kHelRegsProgram) {
		auto accessor = reinterpret_cast<const uintptr_t *>(image);
		*thread->_executor.ip() = accessor[0];
		*thread->_executor.sp() = accessor[1];
	}else if(set == kHelRegsGeneral) {
		auto accessor = reinterpret_cast<const uintptr_t *>(image);
		thread->_executor.general()->rax = accessor[0];
		thread->_executor.general()->rbx = accessor[1];
		thread->_executor.general()->rcx = accessor[2];
		thread->_executor.general()->rdx = accessor[3];
		thread->_executor.general()->rdi = accessor[4];
		thread->_executor.general()->rsi = accessor[5];
		thread->_executor.general()->r8 = accessor[6];
		thread->_executor.general()->r9 = accessor[7];
		thread->_executor.general()->r10 = accessor[8];
		thread->_executor.general()->r11 = accessor[9];
		thread->_executor.general()->r12 = accessor[10];
		thread->_executor.general()->r13 = accessor[11];
		thread->_executor.general()->r14 = accessor[12];
		thread->_executor.general()->r15 = accessor[13];
		thread->_executor.general()->rbp = accessor[14];
	}else if(set == kHelRegsThread) {
		auto accessor = reinterpret_cast<const uintptr_t *>(image);
		thread->_executor.general()->clientFs = accessor[0];
		thread->_executor.general()->clientGs = accessor[1];
	}else if(set == kHelRegsDebug) {
		// FIXME: Make those registers thread-specific.
		auto accessor = reinterpret_cast<const uintptr_t *>(image);
		breakOnWrite(reinterpret_cast<uint32_t *>(*accessor));
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
	*counter = currentNanos();
	return kHelErrNone;
}

HelError helSubmitAwaitClock(uint64_t counter, HelQueue *queue, uintptr_t context) {
	struct Closure : PrecisionTimerNode, QueueNode {
		static void issue(uint64_t ticks, frigg::SharedPtr<AddressSpace> space,
				void *queue, uintptr_t context) {
			auto node = frigg::construct<Closure>(*kernelAlloc, ticks,
					frigg::move(space), queue, context);
			installTimer(node);
		}

		explicit Closure(uint64_t ticks, frigg::SharedPtr<AddressSpace> the_space,
				void *queue, uintptr_t context)
		: PrecisionTimerNode{ticks}, space{frigg::move(the_space)}, queue{queue} {
			setupContext(context);
			setupLength(sizeof(HelSimpleResult));
		}

		void onElapse() override {
			space->queueSpace.submit(space, (uintptr_t)queue, this);
		}

		void emit(ForeignSpaceAccessor accessor) override {
			HelSimpleResult data{translateError(kErrSuccess), 0};
			accessor.copyTo(0, &data, sizeof(HelSimpleResult));

			frigg::destruct(*kernelAlloc, this);
		}

		frigg::SharedPtr<AddressSpace> space;
		void *queue;
	};
	
	auto this_thread = getCurrentThread();

	auto ticks = durationToTicks(0, 0, 0, counter);
	Closure::issue(ticks, this_thread->getAddressSpace().toShared(), queue, context);

	return kHelErrNone;
}

HelError helCreateStream(HelHandle *lane1_handle, HelHandle *lane2_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	auto lanes = createStream();
	{
		Universe::Guard universe_guard(&this_universe->lock);
		*lane1_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(frigg::move(lanes.get<0>())));
		*lane2_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(frigg::move(lanes.get<1>())));
	}

	return kHelErrNone;
}

HelError helSubmitAsync(HelHandle handle, const HelAction *actions, size_t count,
		HelQueue *queue, uintptr_t context, uint32_t flags) {
	(void)flags;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
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

	auto handler = frigg::construct<MsgHandler>(*kernelAlloc, count,
			this_thread->getAddressSpace().toShared(), queue, context);

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
			using Token = SetResult<OfferWriter>;
			LaneHandle branch = target.getStream()->submitOffer(target.getLane(),
					Token(handler, i - 1));

			if(action.flags & kHelItemAncillary)
				stack.push(branch);
		} break;
		case kHelActionAccept: {
			using Token = SetResult<AcceptWriter>;
			LaneHandle branch = target.getStream()->submitAccept(target.getLane(),
					this_universe.toWeak(),
					Token(handler, i - 1));

			if(action.flags & kHelItemAncillary)
				stack.push(branch);
		} break;
		case kHelActionSendFromBuffer: {
			using Token = SetResult<SendStringWriter>;
			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, action.length);
			memcpy(buffer.data(), action.buffer, action.length);
			target.getStream()->submitSendBuffer(target.getLane(), frigg::move(buffer),
					Token(handler, i - 1));
		} break;
		case kHelActionRecvInline: {
			using Token = SetResult<RecvInlineWriter>;
			auto space = this_thread->getAddressSpace().toShared();
			target.getStream()->submitRecvInline(target.getLane(),
					Token(handler, i - 1));
		} break;
		case kHelActionRecvToBuffer: {
			using Token = SetResult<RecvStringWriter>;
			auto space = this_thread->getAddressSpace().toShared();
			auto accessor = ForeignSpaceAccessor::acquire(frigg::move(space),
					action.buffer, action.length);
			target.getStream()->submitRecvBuffer(target.getLane(), frigg::move(accessor),
					Token(handler, i - 1));
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

			using Token = SetResult<PushDescriptorWriter>;
			target.getStream()->submitPushDescriptor(target.getLane(), frigg::move(operand),
					Token(handler, i - 1));
		} break;
		case kHelActionPullDescriptor: {
			using Token = SetResult<PullDescriptorWriter>;
			target.getStream()->submitPullDescriptor(target.getLane(), this_universe.toWeak(),
					Token(handler, i - 1));
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

	struct Blocker : FutexNode {
		Blocker(frigg::UnsafePtr<Thread> thread)
		: _thread{thread}, _complete{false} { }

		void onWake() override {
			_complete.store(true, std::memory_order_release);
			Thread::unblockOther(_thread);
		}

		bool check() {
			return _complete.load(std::memory_order_acquire);
		}
	
	private:
		frigg::UnsafePtr<Thread> _thread;
		std::atomic<bool> _complete;
	};

	Blocker blocker{this_thread};

	// TODO: Support physical (i.e. non-private) futexes.
	space->futexSpace.submitWait(VirtualAddr(pointer), [&] () -> bool {
		auto v = __atomic_load_n(pointer, __ATOMIC_RELAXED);
		return expected == v;
	}, &blocker);

	Thread::blockCurrentWhile([&] {
		return !blocker.check();
	});

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

HelError helAccessIrq(int number, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	auto irq = frigg::makeShared<IrqObject>(*kernelAlloc);
	attachIrq(getGlobalSystemIrq(number), irq.get());

	Universe::Guard universe_guard(&this_universe->lock);
	*handle = this_universe->attachDescriptor(universe_guard,
			IrqDescriptor(frigg::move(irq)));
	universe_guard.unlock();

	return kHelErrNone;
}
HelError helSetupIrq(HelHandle handle, uint32_t flags) {
	assert(!"helSetupIrq is broken and should be removed");
}
HelError helAcknowledgeIrq(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IrqObject> irq;
	{
		Universe::Guard universe_guard(&this_universe->lock);
		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;
	}

	irq->acknowledge();

	return kHelErrNone;
}
HelError helSubmitWaitForIrq(HelHandle handle,
		HelQueue *queue, uintptr_t context) {
	struct Closure : AwaitIrqNode, QueueNode {
		static void issue(frigg::SharedPtr<IrqObject> irq,
				frigg::SharedPtr<AddressSpace> space,
				void *queue, uintptr_t context) {
			auto node = frigg::construct<Closure>(*kernelAlloc,
					frigg::move(space), queue, context);
			irq->submitAwait(node);
		}

	public:
		explicit Closure(frigg::SharedPtr<AddressSpace> space,
				void *queue, uintptr_t context)
		: _space{frigg::move(space)}, _queue{queue} {
			setupContext(context);
			setupLength(sizeof(HelSimpleResult));
		}

		void onRaise(Error error) override {
			_error = error;
			_space->queueSpace.submit(_space, (uintptr_t)_queue, this);
		}

		void emit(ForeignSpaceAccessor accessor) override {
			HelSimpleResult data{translateError(_error), 0};
			accessor.copyTo(0, &data, sizeof(HelSimpleResult));

			frigg::destruct(*kernelAlloc, this);
		}

	private:
		frigg::SharedPtr<AddressSpace> _space;
		void *_queue;
		Error _error;
	};
	
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqObject> irq;
	{
		Universe::Guard universe_guard(&this_universe->lock);

		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;
	}

	Closure::issue(frigg::move(irq),
			this_thread->getAddressSpace().toShared(), queue, context);

	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *user_port_array, size_t num_ports,
		HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	auto io_space = frigg::makeShared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++)
		io_space->addPort(user_port_array[i]);

	Universe::Guard universe_guard(&this_universe->lock);
	*handle = this_universe->attachDescriptor(universe_guard,
			IoDescriptor(frigg::move(io_space)));
	universe_guard.unlock();

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IoSpace> io_space;
	{
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

