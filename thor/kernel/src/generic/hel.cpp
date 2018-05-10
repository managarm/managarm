
#include "kernel.hpp"
#include "irq.hpp"
#include "../arch/x86/debug.hpp"

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

// TODO: one translate function per error source?
HelError translateError(Error error) {
	switch(error) {
	case kErrSuccess: return kHelErrNone;
	case kErrThreadExited: return kHelErrThreadTerminated;
//		case kErrClosedLocally: return kHelErrClosedLocally;
	case kErrClosedRemotely: return kHelErrClosedRemotely;
//		case kErrBufferTooSmall: return kHelErrBufferTooSmall;
	case kErrFault: return kHelErrFault;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}
}

template<typename P>
struct PostEvent {
public:
	struct Wrapper : QueueNode {
		template<typename... Args>
		Wrapper(uintptr_t context, Args &&... args)
		: _writer{frigg::forward<Args>(args)...} {
			setupContext(context);
			setupChunk(&_writer.chunk);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

	private:
		P _writer;
	};

	PostEvent(frigg::SharedPtr<AddressSpace> space, void *queue, uintptr_t context)
	: _space(frigg::move(space)), _queue(queue), _context(context) { }
	
	template<typename... Args>
	void operator() (Args &&... args) {
		auto wrapper = frigg::construct<Wrapper>(*kernelAlloc,
				_context, frigg::forward<Args>(args)...);
		_space->queueSpace.submit(_space, (uintptr_t)_queue, wrapper);
	}

private:
	frigg::SharedPtr<AddressSpace> _space;
	void *_queue;
	uintptr_t _context;
};

struct ManageMemoryWriter {
	ManageMemoryWriter(Error error, uintptr_t offset, size_t length)
	: chunk{&_result, sizeof(HelManageResult), nullptr},
			_result{translateError(error), 0, offset, length} { }

	QueueChunk chunk;

private:
	HelManageResult _result;
};

struct LockMemoryWriter {
	LockMemoryWriter(Error error)
	: chunk{&_result, sizeof(HelSimpleResult), nullptr},
			_result{translateError(error), 0} { }

	QueueChunk chunk;

private:
	HelSimpleResult _result;
};

struct OfferWriter {
	OfferWriter(Error error)
	: _chunk{&_result, sizeof(HelSimpleResult), nullptr},
			_result{translateError(error), 0} { }

	OfferWriter(const OfferWriter &) = delete;

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelSimpleResult _result;
};

struct AcceptWriter {
	AcceptWriter(Error error, frigg::WeakPtr<Universe> weak_universe, LaneDescriptor lane)
	: _chunk{&_result, sizeof(HelHandleResult), nullptr},
			_result{translateError(error), 0, kHelNullHandle} {
		if(weak_universe) {
			auto universe = weak_universe.grab();
			assert(universe);

			auto irq_lock = frigg::guard(&irqMutex());
			Universe::Guard lock(&universe->lock);

			_result.handle = universe->attachDescriptor(lock, frigg::move(lane));
		}
	}

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelHandleResult _result;
};

struct ImbueCredentialsWriter {
	ImbueCredentialsWriter(Error error)
	: _chunk{&_result, sizeof(HelSimpleResult), nullptr},
			_result{translateError(error), 0} { }

	ImbueCredentialsWriter(const ImbueCredentialsWriter &) = delete;

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelSimpleResult _result;
};

struct ExtractCredentialsWriter {
	ExtractCredentialsWriter(Error error, frigg::Array<char, 16> credentials)
	: _chunk{&_result, sizeof(HelCredentialsResult), nullptr},
			_result{translateError(error), 0} {
		memcpy(_result.credentials, credentials.data(), 16);
	}

	ExtractCredentialsWriter(const ExtractCredentialsWriter &) = delete;

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelCredentialsResult _result;
};

struct SendStringWriter {
	SendStringWriter(Error error)
	: _chunk{&_result, sizeof(HelSimpleResult), nullptr},
			_result{translateError(error), 0} { }

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelSimpleResult _result;
};

struct RecvInlineWriter {
	RecvInlineWriter(Error error, frigg::UniqueMemory<KernelAlloc> buffer)
	: _resultChunk{&_result, sizeof(HelInlineResultNoFlex), &_bufferChunk},
			_bufferChunk{buffer.data(), buffer.size(), nullptr},
			_result{translateError(error), 0, buffer.size()},
			_buffer(frigg::move(buffer)) { }

	QueueChunk *chunk() {
		return &_resultChunk;
	}

	void link(QueueChunk *next) {
		_bufferChunk.link = next;
	}

private:
	QueueChunk _resultChunk;
	QueueChunk _bufferChunk;
	HelInlineResultNoFlex _result;
	frigg::UniqueMemory<KernelAlloc> _buffer;
};

struct RecvStringWriter {
	RecvStringWriter(Error error, size_t length)
	: _chunk{&_result, sizeof(HelLengthResult), nullptr},
			_result{translateError(error), 0, length} { }

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelLengthResult _result;
};

struct PushDescriptorWriter {
	PushDescriptorWriter(Error error)
	: _chunk{&_result, sizeof(HelSimpleResult), nullptr},
			_result{translateError(error), 0} { }

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelSimpleResult _result;
};

struct PullDescriptorWriter {
	PullDescriptorWriter(Error error, frigg::WeakPtr<Universe> weak_universe,
			AnyDescriptor descriptor)
	: _chunk{&_result, sizeof(HelHandleResult), nullptr},
			_result{translateError(error), 0, kHelNullHandle} {
		if(weak_universe) {
			auto universe = weak_universe.grab();
			assert(universe);

			auto irq_lock = frigg::guard(&irqMutex());
			Universe::Guard lock(&universe->lock);

			_result.handle = universe->attachDescriptor(lock, frigg::move(descriptor));
		}
	}

	QueueChunk *chunk() {
		return &_chunk;
	}

	void link(QueueChunk *next) {
		_chunk.link = next;
	}

private:
	QueueChunk _chunk;
	HelHandleResult _result;
};

struct ObserveThreadWriter {
	ObserveThreadWriter(Error error, Interrupt interrupt)
	: chunk{&_result, sizeof(HelObserveResult), nullptr},
			_result{translateError(error), 0, 0} {
		if(interrupt == kIntrNull) {
			_result.observation = kHelObserveNull;
		}else if(interrupt == kIntrRequested) {
			_result.observation = kHelObserveInterrupt;
		}else if(interrupt == kIntrPanic) {
			_result.observation = kHelObservePanic;
		}else if(interrupt == kIntrBreakpoint) {
			_result.observation = kHelObserveBreakpoint;
		}else if(interrupt == kIntrGeneralFault) {
			_result.observation = kHelObserveGeneralFault;
		}else if(interrupt == kIntrPageFault) {
			_result.observation = kHelObservePageFault;
		}else if(interrupt >= kIntrSuperCall) {
			_result.observation = kHelObserveSuperCall + (interrupt - kIntrSuperCall);
		}else{
			frigg::panicLogger() << "Unexpected interrupt" << frigg::endLog;
			__builtin_unreachable();
		}
	}

	QueueChunk chunk;

private:
	HelObserveResult _result;
};

using ItemWriter = frigg::Variant<
	OfferWriter,
	AcceptWriter,
	ImbueCredentialsWriter,
	ExtractCredentialsWriter,
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
	: _numItems(num_items), _results(frigg::constructN<ItemWriter>(*kernelAlloc, num_items)),
			_numComplete(0), _space(frigg::move(space)), _queue(queue) {
		setupContext(context);
	}

private:
	void submit() {
		auto chunk = [this] (int index) {
			return _results[index].apply([&] (auto &item) {
				return item.chunk();
			});
		};
		
		auto link = [this] (int index, QueueChunk *next) {
			return _results[index].apply([&] (auto &item) {
				return item.link(next);
			});
		};
		
		for(size_t i = 1; i < _numItems; ++i)
			link(i - 1, chunk(i));
		
		assert(_numItems);
		setupChunk(chunk(0));
		_space->queueSpace.submit(_space, (uintptr_t)_queue, this);
	}
	
	void complete() override {
		// TODO: Destruct this.
	}

	size_t _numItems;
	ItemWriter *_results;
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
		_handler->_results[_index].emplace<W>(frigg::forward<Args>(args)...);
		auto c = _handler->_numComplete.fetch_add(1, std::memory_order_acq_rel);
		if(c + 1 == _handler->_numItems)
			_handler->submit();
	}

private:
	MsgHandler *_handler;
	size_t _index;
};

HelError helLog(const char *string, size_t length) {
	for(size_t i = 0; i < length; i++)
		infoSink.print(readUserObject<char>(string + i));

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

		*out_handle = universe->attachDescriptor(lock, frigg::move(descriptor));
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
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryAccessDescriptor(frigg::move(memory)));
	}

	return kHelErrNone;
}

HelError helResizeMemory(HelHandle handle, size_t new_size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryAccessDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryAccessDescriptor>().memory;
	}

	memory->resize(new_size);

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
		auto irq_lock = frigg::guard(&irqMutex());
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
		auto irq_lock = frigg::guard(&irqMutex());
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
	
	auto irq_lock = frigg::guard(&irqMutex());
	Universe::Guard universe_guard(&this_universe->lock);

	*handle = this_universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(frigg::move(space)));

	return kHelErrNone;
}

HelError helForkSpace(HelHandle handle, HelHandle *forked_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
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

	frigg::SharedPtr<AddressSpace> forked;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		forked = space->fork(space_guard);
	}
	
	{
		auto irq_lock = frigg::guard(&irqMutex());
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
		auto irq_lock = frigg::guard(&irqMutex());
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

	if(flags & kHelMapProtRead)
		map_flags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		map_flags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		map_flags |= AddressSpace::kMapProtExecute;

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
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		space->map(space_guard, memory, (VirtualAddr)pointer, offset, length,
				map_flags, &actual_address);
	}

	// TODO: This should not be necessary but we get a page fault if we disable this.
	// Investigate that bug!
	thorRtInvalidateSpace();

	*actual_pointer = (void *)actual_address;

	return kHelErrNone;
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

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
	
	auto closure = frigg::construct<AddressUnmapNode>(*kernelAlloc);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		space->unmap(space_guard, (VirtualAddr)pointer, length, closure);
	}

	return kHelErrNone;
}

HelError helPointerPhysical(void *pointer, uintptr_t *physical) {
	auto this_thread = getCurrentThread();
	
	frigg::SharedPtr<AddressSpace> space = this_thread->getAddressSpace().toShared();

	auto address = (VirtualAddr)pointer;
	auto misalign = address % kPageSize;

	PhysicalAddr page_physical;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		page_physical = space->grabPhysical(space_guard, address - misalign);
		assert(page_physical != PhysicalAddr(-1));
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
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(wrapper->is<AddressSpaceDescriptor>()) {
			space = wrapper->get<AddressSpaceDescriptor>().space;
		}else if(wrapper->is<ThreadDescriptor>()) {
			auto thread = wrapper->get<ThreadDescriptor>().thread;
			space = thread->getAddressSpace().toShared();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	// TODO: This enableUserAccess() should be replaced by a writeUserMemory().
	auto accessor = ForeignSpaceAccessor::acquire(frigg::move(space),
			(void *)address, length);
	enableUserAccess();
	accessor.load(0, buffer, length);
	disableUserAccess();

	return kHelErrNone;
}

HelError helStoreForeign(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
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
			space = thread->getAddressSpace().toShared();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	// TODO: This enableUserAccess() should be replaced by a readUserMemory().
	auto accessor = ForeignSpaceAccessor::acquire(frigg::move(space),
			(void *)address, length);
	enableUserAccess();
	auto error = accessor.write(0, buffer, length);
	assert(!error);
	disableUserAccess();

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
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
		auto irq_lock = frigg::guard(&irqMutex());
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
		auto irq_lock = frigg::guard(&irqMutex());
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
		auto irq_lock = frigg::guard(&irqMutex());
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
		auto irq_lock = frigg::guard(&irqMutex());
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

	Scheduler::associate(new_thread.get(), localScheduler());
	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(new_thread);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(frigg::move(new_thread)));
	}

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

HelError helSubmitObserve(HelHandle handle, HelQueue *queue, uintptr_t context) {
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
	
	// TODO: protect the thread with a lock!
	PostEvent<ObserveThreadWriter> functor{this_thread->getAddressSpace().toShared(), queue, context};
	thread->submitObserve(frigg::move(functor));

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

	Thread::resumeOther(thread);

	return kHelErrNone;
}

HelError helLoadRegisters(HelHandle handle, int set, void *image) {
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

	if(set == kHelRegsProgram) {
		uintptr_t regs[2];
		regs[0] = *thread->_executor.ip();
		regs[1] = *thread->_executor.sp();
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2);
	}else if(set == kHelRegsGeneral) {
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
		uintptr_t regs[2];
		regs[0] = thread->_executor.general()->clientFs;
		regs[1] = thread->_executor.general()->clientGs;
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2);
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		// FIXME: Properly handle this below.
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
	
	// FIXME: We need to lock the thread and ensure it is in the interrupted state.
	if(set == kHelRegsProgram) {
		uintptr_t regs[2];
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2);
		*thread->_executor.ip() = regs[0];
		*thread->_executor.sp() = regs[1];
	}else if(set == kHelRegsGeneral) {
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
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2);
		thread->_executor.general()->clientFs = regs[0];
		thread->_executor.general()->clientGs = regs[1];
	}else if(set == kHelRegsDebug) {
		// FIXME: Make those registers thread-specific.
		auto reg = readUserObject(reinterpret_cast<uint32_t *const *>(image));
		breakOnWrite(reg);
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
		: PrecisionTimerNode{ticks}, space{frigg::move(the_space)}, queue{queue},
				chunk{&result, sizeof(HelSimpleResult), nullptr},
				result{translateError(kErrSuccess), 0} {
			setupContext(context);
			setupChunk(&chunk);
		}

		void onElapse() override {
			space->queueSpace.submit(space, (uintptr_t)queue, this);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		frigg::SharedPtr<AddressSpace> space;
		void *queue;
		QueueChunk chunk;
		HelSimpleResult result;
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
		auto irq_lock = frigg::guard(&irqMutex());
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
		auto irq_lock = frigg::guard(&irqMutex());
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
		HelAction action = readUserObject(actions + i++);

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
		case kHelActionImbueCredentials: {
			using Token = SetResult<ImbueCredentialsWriter>;
			target.getStream()->submitImbueCredentials(target.getLane(),
					this_thread->credentials(),
					Token(handler, i - 1));
		} break;
		case kHelActionExtractCredentials: {
			using Token = SetResult<ExtractCredentialsWriter>;
			target.getStream()->submitExtractCredentials(target.getLane(),
					Token(handler, i - 1));
		} break;
		case kHelActionSendFromBuffer: {
			using Token = SetResult<SendStringWriter>;
			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, action.length);
			readUserMemory(buffer.data(), action.buffer, action.length);
			target.getStream()->submitSendBuffer(target.getLane(), frigg::move(buffer),
					Token(handler, i - 1));
		} break;
		case kHelActionSendFromBufferSg: {
			using Token = SetResult<SendStringWriter>;
			
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
				auto irq_lock = frigg::guard(&irqMutex());
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
		enableUserAccess();
		auto v = __atomic_load_n(pointer, __ATOMIC_RELAXED);
		disableUserAccess();
		return expected == v;
	}, &blocker);

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

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IrqDescriptor(frigg::move(irq)));
	}

	return kHelErrNone;
}
HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence) {
	assert(!(flags & ~(kHelAckKick)));

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
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

	if(flags & kHelAckKick) {
		irq->acknowledge();
	}else{
		assert(!flags);
		irq->acknowledge();
	}

	return kHelErrNone;
}
HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		HelQueue *queue, uintptr_t context) {
	struct Closure : AwaitIrqNode, QueueNode {
		static void issue(frigg::SharedPtr<IrqObject> irq, uint64_t sequence,
				frigg::SharedPtr<AddressSpace> space,
				void *queue, uintptr_t context) {
			auto node = frigg::construct<Closure>(*kernelAlloc,
					frigg::move(space), queue, context);
			irq->submitAwait(node, sequence);
		}

	public:
		explicit Closure(frigg::SharedPtr<AddressSpace> space,
				void *queue, uintptr_t context)
		: _space{frigg::move(space)}, _queue{queue},
				chunk{&result, sizeof(HelEventResult), nullptr} {
			setupContext(context);
			setupChunk(&chunk);
		}

		void onRaise(Error error, uint64_t sequence) override {
			result.error = translateError(error);
			result.sequence = sequence;
			_space->queueSpace.submit(_space, (uintptr_t)_queue, this);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

	private:
		frigg::SharedPtr<AddressSpace> _space;
		void *_queue;
		QueueChunk chunk;
		HelEventResult result;
	};
	
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

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

	Closure::issue(frigg::move(irq), sequence,
			this_thread->getAddressSpace().toShared(), queue, context);

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
				IoDescriptor(frigg::move(io_space)));
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

