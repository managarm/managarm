#include <string.h>
#include <cstddef>

#include <async/algorithm.hpp>
#include <async/cancellation.hpp>
#include <frg/container_of.hpp>
#include <frg/dyn_array.hpp>
#include <frg/small_vector.hpp>
#include <thor-internal/event.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/ipc-queue.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/kernlet.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/random.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>
#ifdef __x86_64__
#include <thor-internal/arch/debug.hpp>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/arch/npt.hpp>
#include <thor-internal/arch/svm.hpp>
#endif
#include <hel.h>

using namespace thor;

namespace {
	// TODO: Replace this by a function that returns the type of special descriptor.
	bool isSpecialMemoryView(HelHandle handle) {
		return handle == kHelZeroMemory;
	}

	smarter::shared_ptr<MemoryView> getSpecialMemoryView(HelHandle handle) {
		assert(handle == kHelZeroMemory);
		return getZeroMemory();
	}
}

extern "C" int doCopyFromUser(void *dest, const void *src, size_t size);
extern "C" int doCopyToUser(void *dest, const void *src, size_t size);
extern "C" int doAtomicUserLoad(unsigned int *out, const unsigned int *p);

bool readUserMemory(void *kernelPtr, const void *userPtr, size_t size) {
	uintptr_t limit;
	if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(userPtr), size, &limit))
		return false;
	if(inHigherHalf(limit))
		return false;
	enableUserAccess();
	int e = doCopyFromUser(kernelPtr, userPtr, size);
	disableUserAccess();
	return !e;
}

bool writeUserMemory(void *userPtr, const void *kernelPtr, size_t size) {
	uintptr_t limit;
	if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(userPtr), size, &limit))
		return false;
	if(inHigherHalf(limit))
		return false;
	enableUserAccess();
	int e = doCopyToUser(userPtr, kernelPtr, size);
	disableUserAccess();
	return !e;
}

template<typename T>
bool readUserObject(const T *pointer, T &object) {
	return readUserMemory(&object, pointer, sizeof(T));
}

template<typename T>
bool writeUserObject(T *pointer, T object) {
	return writeUserMemory(pointer, &object, sizeof(T));
}

template<typename T>
bool readUserArray(const T *pointer, T *array, size_t count) {
	size_t size;
	if(__builtin_mul_overflow(sizeof(T), count, &size))
		return false;
	return readUserMemory(array, pointer, size);
}

template<typename T>
bool writeUserArray(T *pointer, const T *array, size_t count) {
	size_t size;
	if(__builtin_mul_overflow(sizeof(T), count, &size))
		return false;
	return writeUserMemory(pointer, array, size);
}

size_t ipcSourceSize(size_t size) {
	return (size + 7) & ~size_t(7);
}

// TODO: one translate function per error source?
HelError translateError(Error error) {
	switch(error) {
	case Error::success: return kHelErrNone;
	case Error::threadExited: return kHelErrThreadTerminated;
	case Error::transmissionMismatch: return kHelErrTransmissionMismatch;
	case Error::laneShutdown: return kHelErrLaneShutdown;
	case Error::endOfLane: return kHelErrEndOfLane;
	case Error::dismissed: return kHelErrDismissed;
	case Error::bufferTooSmall: return kHelErrBufferTooSmall;
	case Error::fault: return kHelErrFault;
	case Error::remoteFault: return kHelErrRemoteFault;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}
}

HelError helLog(const char *string, size_t length) {
	size_t offset = 0;
	while(offset < length) {
		LogMessage log;
		auto chunk = frg::min(length - offset, size_t{logLineLength});

		if(!readUserArray(string + offset, log.text, chunk))
			return kHelErrFault;

		auto p = infoLogger();
		for(size_t i = 0; i < chunk; i++)
			p << frg::char_fmt(log.text[i]);
		p << frg::endlog;

		offset += chunk;
	}

	return kHelErrNone;
}

HelError helNop() {
	return kHelErrNone;
}

HelError helSubmitAsyncNop(HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	[] (smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(queue), context);

	return kHelErrNone;
}

HelError helCreateUniverse(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto new_universe = smarter::allocate_shared<Universe>(*kernelAlloc);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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
	smarter::shared_ptr<Universe> universe;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard lock(this_universe->lock);

		auto descriptor_it = this_universe->getDescriptor(lock, handle);
		if(!descriptor_it)
			return kHelErrNoDescriptor;
		descriptor = *descriptor_it;

		if(universe_handle == kHelThisUniverse) {
			universe = this_universe.lock();
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
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard lock(universe->lock);

		*out_handle = universe->attachDescriptor(lock, std::move(descriptor));
	}
	return kHelErrNone;
}

HelError helDescriptorInfo(HelHandle handle, HelDescriptorInfo *) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq_lock = frg::guard(&irqMutex());
	Universe::Guard universe_guard(this_universe->lock);

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
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();
	assert(!flags);

	smarter::shared_ptr<Credentials> creds;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(thisUniverse->lock);

		if(handle == kHelThisThread) {
			creds = thisThread.lock();
		}else{
			auto wrapper = thisUniverse->getDescriptor(universe_guard, handle);
			if(!wrapper)
				return kHelErrNoDescriptor;
			if(wrapper->is<ThreadDescriptor>())
				creds = remove_tag_cast(wrapper->get<ThreadDescriptor>().thread);
			else if(wrapper->is<LaneDescriptor>())
				creds = wrapper->get<LaneDescriptor>().handle.getStream().lock();
			else
				return kHelErrBadDescriptor;
		}
	}

	if(!writeUserMemory(credentials, creds->credentials(), 16))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle universeHandle, HelHandle handle) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<Universe> universe;
	if(universeHandle == kHelThisUniverse) {
		universe = thisUniverse.lock();
	}else{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeLock(thisUniverse->lock);

		auto universeIt = thisUniverse->getDescriptor(universeLock, universeHandle);
		if(!universeIt)
			return kHelErrNoDescriptor;
		if(!universeIt->is<UniverseDescriptor>())
			return kHelErrBadDescriptor;
		universe = universeIt->get<UniverseDescriptor>().universe;
	}

	frg::optional<AnyDescriptor> descriptor;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard otherUniverseLock(universe->lock);

		descriptor = universe->detachDescriptor(otherUniverseLock, handle);
	}
	if(!descriptor)
		return kHelErrNoDescriptor;

	// Note that the descriptor is released outside of the locks.

	return kHelErrNone;
}

HelError helCreateQueue(HelQueueParameters *paramsPtr, HelHandle *handle) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	HelQueueParameters params;
	if(!readUserObject(paramsPtr, params))
		return kHelErrFault;

	if(params.flags)
		return kHelErrIllegalArgs;

	auto queue = smarter::allocate_shared<IpcQueue>(*kernelAlloc,
			params.ringShift, params.numChunks, params.chunkSize);
	queue->setupSelfPtr(queue);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(thisUniverse->lock);

		*handle = thisUniverse->attachDescriptor(universe_guard,
				QueueDescriptor(std::move(queue)));
	}

	return kHelErrNone;
}

HelError helCancelAsync(HelHandle handle, uint64_t async_id) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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
	if(!size)
		return kHelErrIllegalArgs;
	if(size & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

//	auto pressure = physicalAllocator->numUsedPages() * kPageSize;
//	infoLogger() << "Allocate " << (void *)size
//			<< ", sum of allocated memory: " << (void *)pressure << frg::endlog;

	HelAllocRestrictions effective{
		.addressBits = 64
	};
	if(restrictions)
		if(!readUserMemory(&effective, restrictions, sizeof(HelAllocRestrictions)))
			return kHelErrFault;

	smarter::shared_ptr<AllocatedMemory> memory;
	if(flags & kHelAllocContinuous) {
		memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits,
				size, kPageSize);
	}else if(flags & kHelAllocOnDemand) {
		memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits);
	}else{
		// TODO: 
		memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, size, effective.addressBits);
	}
	memory->selfPtr = memory;

	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		*handle = thisUniverse->attachDescriptor(universeGuard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helResizeMemory(HelHandle handle, size_t newSize) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryViewDescriptor>().memory;
	}

	Thread::asyncBlockCurrent([] (smarter::shared_ptr<MemoryView> memory, size_t newSize)
			-> coroutine<void> {
		co_await memory->resize(newSize);
	}(std::move(memory), newSize));

	return kHelErrNone;
}

HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle) {
	if(flags & ~uint32_t{kHelManagedReadahead})
		return kHelErrIllegalArgs;
	if(size & (kPageSize - 1))
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	auto managed = smarter::allocate_shared<ManagedSpace>(*kernelAlloc, size,
			flags & kHelManagedReadahead);
	managed->selfPtr = managed;
	auto backingMemory = smarter::allocate_shared<BackingMemory>(*kernelAlloc, managed);
	auto frontalMemory = smarter::allocate_shared<FrontalMemory>(*kernelAlloc, std::move(managed));
	frontalMemory->selfPtr = frontalMemory;

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(thisUniverse->lock);

		*backing_handle = thisUniverse->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(backingMemory)));
		*frontal_handle = thisUniverse->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(frontalMemory)));
	}

	return kHelErrNone;
}

HelError helCopyOnWrite(HelHandle memoryHandle,
		uintptr_t offset, size_t size, HelHandle *outHandle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<MemoryView> view;

	if(memoryHandle < 0) {
		if(!isSpecialMemoryView(memoryHandle))
			return kHelErrBadDescriptor;
		view = getSpecialMemoryView(memoryHandle);
	}

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		if(memoryHandle >= 0) {
			auto wrapper = this_universe->getDescriptor(universe_guard, memoryHandle);
			if(!wrapper)
				return kHelErrNoDescriptor;
			if(!wrapper->is<MemoryViewDescriptor>())
				return kHelErrBadDescriptor;
			view = wrapper->get<MemoryViewDescriptor>().memory;
		}
	}

	auto slice = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc, std::move(view),
			offset, size);
	slice->selfPtr = slice;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	auto memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc, physical, size,
			CachingMode::null);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateIndirectMemory(size_t numSlots, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = smarter::allocate_shared<IndirectMemory>(*kernelAlloc, numSlots);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(std::move(memory)));
	}

	return kHelErrNone;
}

HelError helAlterMemoryIndirection(HelHandle indirectHandle, size_t slot,
		HelHandle memoryHandle, uintptr_t offset, size_t size) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<MemoryView> indirectView;
	smarter::shared_ptr<MemoryView> memoryView;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeLock(thisUniverse->lock);

		auto indirectWrapper = thisUniverse->getDescriptor(universeLock, indirectHandle);
		if(!indirectWrapper)
			return kHelErrNoDescriptor;
		if(indirectWrapper->is<MemoryViewDescriptor>())
			indirectView = indirectWrapper->get<MemoryViewDescriptor>().memory;
		else
			return kHelErrBadDescriptor;

		auto memoryWrapper = thisUniverse->getDescriptor(universeLock, memoryHandle);
		if(!memoryWrapper)
			return kHelErrNoDescriptor;
		if(memoryWrapper->is<MemoryViewDescriptor>())
			memoryView = memoryWrapper->get<MemoryViewDescriptor>().memory;
		else if(memoryWrapper->is<MemorySliceDescriptor>())
			memoryView = memoryWrapper->get<MemorySliceDescriptor>().slice->getView();
		else
			return kHelErrBadDescriptor;
	}

	if(auto e = indirectView->setIndirection(slot, std::move(memoryView), offset, size);
			e != Error::success) {
		if(e == Error::illegalObject) {
			return kHelErrUnsupportedOperation;
		}else{
			assert(e == Error::outOfBounds);
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

	smarter::shared_ptr<MemoryView> view;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, memoryHandle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = wrapper->get<MemoryViewDescriptor>().memory;
	}

	auto slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
			std::move(view), offset, size);
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemorySliceDescriptor(std::move(slice)));
	}

	return kHelErrNone;
}

HelError helForkMemory(HelHandle handle, HelHandle *forkedHandle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<MemoryView> view;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto viewWrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!viewWrapper)
			return kHelErrNoDescriptor;
		if(!viewWrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		view = viewWrapper->get<MemoryViewDescriptor>().memory;
	}

	auto [error, forkedView] = Thread::asyncBlockCurrent(view->fork());

	if(error == Error::illegalObject)
		return kHelErrUnsupportedOperation;
	assert(error == Error::success);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*forkedHandle = this_universe->attachDescriptor(universe_guard,
				MemoryViewDescriptor(forkedView));
	}

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto space = AddressSpace::create();

	auto irq_lock = frg::guard(&irqMutex());
	Universe::Guard universe_guard(this_universe->lock);

	*handle = this_universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(std::move(space)));

	return kHelErrNone;
}

HelError helCreateVirtualizedSpace(HelHandle *handle) {
#ifdef __x86_64__
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto irq_lock = frg::guard(&irqMutex());
	auto this_universe = this_thread->getUniverse();

	PhysicalAddr pml4e = physicalAllocator->allocate(kPageSize);
	if(pml4e == static_cast<PhysicalAddr>(-1)) {
		return kHelErrNoMemory;
	}
	PageAccessor paccessor{pml4e};
	memset(paccessor.get(), 0, kPageSize);

	smarter::shared_ptr<VirtualizedPageSpace> vspace;
	if(getGlobalCpuFeatures()->haveVmx) {
		vspace = thor::vmx::EptSpace::create(pml4e);
	} else if(getGlobalCpuFeatures()->haveSvm) {
		vspace = thor::svm::NptSpace::create(pml4e);
	} else {
		physicalAllocator->free(pml4e, kPageSize);
		return kHelErrNoHardwareSupport;
	}

	Universe::Guard universe_guard(this_universe->lock);
	*handle = this_universe->attachDescriptor(universe_guard,
			VirtualizedSpaceDescriptor(std::move(vspace)));
	return kHelErrNone;
#else
	return kHelErrNoHardwareSupport;
#endif
}

HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out) {
#ifdef __x86_64__
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto irq_lock = frg::guard(&irqMutex());
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	Universe::Guard universe_guard(this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!wrapper->is<VirtualizedSpaceDescriptor>())
		return kHelErrBadDescriptor;
	auto space = wrapper->get<VirtualizedSpaceDescriptor>();

	smarter::shared_ptr<VirtualizedCpu> vcpu;
	if(getGlobalCpuFeatures()->haveVmx)
		vcpu = smarter::allocate_shared<vmx::Vmcs>(Allocator{}, (smarter::static_pointer_cast<thor::vmx::EptSpace>(space.space)));
	else if(getGlobalCpuFeatures()->haveSvm)
		vcpu = smarter::allocate_shared<svm::Vcpu>(Allocator{}, (smarter::static_pointer_cast<thor::svm::NptSpace>(space.space)));
	else
		return kHelErrNoHardwareSupport;

	*out = this_universe->attachDescriptor(universe_guard,
			VirtualizedCpuDescriptor(std::move(vcpu)));
	return kHelErrNone;
#else
	return kHelErrNoHardwareSupport;
#endif
}

HelError helRunVirtualizedCpu(HelHandle handle, HelVmexitReason *exitInfo) {
	if(!getCpuData()->haveVirtualization) {
		return kHelErrNoHardwareSupport;
	}
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	Universe::Guard universe_guard(this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	if(!wrapper->is<VirtualizedCpuDescriptor>())
		return kHelErrBadDescriptor;
	auto cpu = wrapper->get<VirtualizedCpuDescriptor>();
	auto info = cpu.vcpu->run();
	if(!writeUserObject(exitInfo, info))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helGetRandomBytes(void *buffer, size_t wantedSize, size_t *actualSize) {
	char bounceBuffer[128];
	size_t generatedSize = generateRandomBytes(bounceBuffer,
			frg::min(wantedSize, size_t{128}));

	if(!writeUserMemory(buffer, bounceBuffer, generatedSize))
		return kHelErrFault;

	*actualSize = generatedSize;
	return kHelErrNone;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, uintptr_t offset, size_t length, uint32_t flags, void **actualPointer) {
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
	if(flags & kHelMapFixed) {
		map_flags |= AddressSpace::kMapFixed;
	}else if(flags & kHelMapFixedNoReplace) {
		map_flags |= AddressSpace::kMapFixedNoReplace;
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

	smarter::shared_ptr<MemorySlice> slice;
	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	smarter::shared_ptr<VirtualSpace> vspace;
	bool isVspace = false;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, memory_handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(memory_wrapper->is<MemorySliceDescriptor>()) {
			slice = memory_wrapper->get<MemorySliceDescriptor>().slice;
		}else if(memory_wrapper->is<MemoryViewDescriptor>()) {
			auto memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
			auto sliceLength = memory->getLength();
			slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
					std::move(memory), 0, sliceLength);
		}else if(memory_wrapper->is<QueueDescriptor>()) {
			auto memory = memory_wrapper->get<QueueDescriptor>().queue->getMemory();
			auto sliceLength = memory->getLength();
			slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
					std::move(memory), 0, sliceLength);
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

	frg::expected<Error, VirtualAddr> mapResult;
	if(!isVspace) {
		if(map_flags & AddressSpace::kMapFixed && !pointer)
			return kHelErrIllegalArgs; // Non-vspaces aren't allowed to map at NULL
		
		mapResult = Thread::asyncBlockCurrent(space->map(slice,
				(VirtualAddr)pointer, offset, length, map_flags));
	} else {
		mapResult = Thread::asyncBlockCurrent(vspace->map(slice,
				(VirtualAddr)pointer, offset, length, map_flags));
	}

	if(!mapResult) {
		assert(mapResult.error() == Error::bufferTooSmall || mapResult.error() == Error::alreadyExists || mapResult.error() == Error::noMemory);

		if(mapResult.error() == Error::bufferTooSmall)
			return kHelErrBufferTooSmall;
		else if(mapResult.error() == Error::noMemory)
			return kHelErrNoMemory;
		else if(mapResult.error() == Error::alreadyExists)
			return kHelErrAlreadyExists;
	}

	*actualPointer = (void *)mapResult.value();
	return kHelErrNone;
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
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	if(!queue->validSize(ipcSourceSize(sizeof(HelSimpleResult))))
		return kHelErrQueueTooSmall;

	[](smarter::shared_ptr<AddressSpace, BindableHandle> space,
			smarter::shared_ptr<IpcQueue> queue,
			VirtualAddr pointer, size_t length,
			uint32_t protectFlags, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		auto outcome = co_await space->protect(pointer, length, protectFlags);
		// TODO: handle errors after propagating them through VirtualSpace::protect.
		assert(outcome);

		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(space), std::move(queue), reinterpret_cast<VirtualAddr>(pointer),
			length, protectFlags, context);

	return kHelErrNone;
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	auto outcome = Thread::asyncBlockCurrent(space->unmap((VirtualAddr)pointer, length));
	if(!outcome) {
		assert(outcome.error() == Error::illegalArgs);
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helSubmitSynchronizeSpace(HelHandle spaceHandle, void *pointer, size_t length,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		if(spaceHandle == kHelNullHandle) {
			space = thisThread->getAddressSpace().lock();
		}else{
			auto spaceWrapper = thisUniverse->getDescriptor(universeGuard, spaceHandle);
			if(!spaceWrapper)
				return kHelErrNoDescriptor;
			if(!spaceWrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = spaceWrapper->get<AddressSpaceDescriptor>().space;
		}

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	[] (smarter::shared_ptr<AddressSpace, BindableHandle> space,
			void *pointer, size_t length,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		auto outcome = co_await space->synchronize((VirtualAddr)pointer, length);
		// TODO: handle errors after propagating them through VirtualSpace::synchronize.
		assert(outcome);

		HelSimpleResult helResult{.error = kHelErrNone, .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(space), pointer, length, std::move(queue), context);

	return kHelErrNone;
}

HelError helPointerPhysical(const void *pointer, uintptr_t *physical) {
	auto thisThread = getCurrentThread();
	auto space = thisThread->getAddressSpace().lock();

	auto disp = (reinterpret_cast<uintptr_t>(pointer) & (kPageSize - 1));
	auto pageAddress = reinterpret_cast<VirtualAddr>(pointer) - disp;

	auto physicalOrError = Thread::asyncBlockCurrent(space->retrievePhysical(pageAddress,
			thisThread->mainWorkQueue()->take()));
	if(!physicalOrError) {
		assert(physicalOrError.error() == Error::fault);
		return kHelErrFault;
	}

	*physical = physicalOrError.value() + disp;

	return kHelErrNone;
}

HelError helSubmitReadMemory(HelHandle handle, uintptr_t address,
		size_t length, void *buffer,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	AnyDescriptor descriptor;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		auto wrapper = thisUniverse->getDescriptor(universeGuard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	auto readMemoryView = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<MemoryView> view,
			uintptr_t address, size_t length, void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{128});
				auto copyOutcome = co_await view->copyFrom(address + progress, temp, chunk,
						submitThread->mainWorkQueue()->take());
				if(!copyOutcome) {
					error = copyOutcome.error();
					break;
				}

				// Enter the submitter's work-queue so that we can access memory directly.
				co_await submitThread->mainWorkQueue()->schedule();

				if(!writeUserMemory(reinterpret_cast<char *>(buffer) + progress, temp, chunk)) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		assert(error == Error::success);
		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto readAddressSpace = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<AddressSpace, BindableHandle> space,
			uintptr_t address, size_t length, void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{128});

				auto outcome = co_await space->readSpace(address + progress, temp, chunk,
						submitThread->mainWorkQueue()->take());
				if(!outcome) {
					error = Error::fault;
					break;
				}

				// Enter the submitter's work-queue so that we can access memory directly.
				co_await submitThread->mainWorkQueue()->schedule();

				if(!writeUserMemory(reinterpret_cast<char *>(buffer) + progress, temp, chunk)) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto readVirtualizedSpace = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<VirtualizedPageSpace> space,
			uintptr_t address, size_t length, void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		// Enter the submitter's work-queue so that we can access memory directly.
		co_await submitThread->mainWorkQueue()->schedule();

		enableUserAccess();
		auto error = space->load(address, length, buffer);
		disableUserAccess();
		assert(error == Error::success || error == Error::fault);

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	if(descriptor.is<MemoryViewDescriptor>()) {
		auto view = descriptor.get<MemoryViewDescriptor>().memory;
		readMemoryView(thisThread.lock(),
				std::move(view), address, length, buffer, std::move(queue), context);
	}else if(descriptor.is<AddressSpaceDescriptor>()) {
		auto space = descriptor.get<AddressSpaceDescriptor>().space;
		readAddressSpace(thisThread.lock(),
				std::move(space), address, length, buffer, std::move(queue), context);
	}else if(descriptor.is<ThreadDescriptor>()) {
		auto thread = descriptor.get<ThreadDescriptor>().thread;
		auto space = thread->getAddressSpace().lock();
		readAddressSpace(thisThread.lock(),
				std::move(space), address, length, buffer, std::move(queue), context);
	}else if(descriptor.is<VirtualizedSpaceDescriptor>()) {
		auto space = descriptor.get<VirtualizedSpaceDescriptor>().space;
		readVirtualizedSpace(thisThread.lock(),
				std::move(space), address, length, buffer, std::move(queue), context);
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helSubmitWriteMemory(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	AnyDescriptor descriptor;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		auto wrapper = thisUniverse->getDescriptor(universeGuard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		descriptor = *wrapper;

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	auto writeMemoryView = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<MemoryView> view,
			uintptr_t address, size_t length, const void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{128});

				// Enter the submitter's work-queue so that we can access memory directly.
				co_await submitThread->mainWorkQueue()->schedule();

				if(!readUserMemory(temp,
						reinterpret_cast<const char *>(buffer) + progress, chunk)) {
					error = Error::fault;
					break;
				}

				auto copyOutcome = co_await view->copyTo(address + progress, temp, chunk,
						submitThread->mainWorkQueue()->take());
				if(!copyOutcome) {
					error = copyOutcome.error();
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto writeAddressSpace = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<AddressSpace, BindableHandle> space,
			uintptr_t address, size_t length, const void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		// Make sure that the pointer arithmetic below does not overflow.
		uintptr_t limit;
		if(__builtin_add_overflow(reinterpret_cast<uintptr_t>(buffer), length, &limit)) {
			HelSimpleResult helResult{.error = kHelErrIllegalArgs, .reserved = {}};
			QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}
		(void)limit;

		Error error = Error::success;
		{
			char temp[128];
			size_t progress = 0;
			while(progress < length) {
				auto chunk = frg::min(length - progress, size_t{128});

				// Enter the submitter's work-queue so that we can access memory directly.
				co_await submitThread->mainWorkQueue()->schedule();
				if(!readUserMemory(temp,
						reinterpret_cast<const char *>(buffer) + progress, chunk)) {
					error = Error::fault;
					break;
				}

				auto outcome = co_await space->writeSpace(address + progress, temp, chunk,
						submitThread->mainWorkQueue()->take());
				if(!outcome) {
					error = Error::fault;
					break;
				}
				progress += chunk;
			}
		}

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	auto writeVirtualizedSpace = [] (smarter::shared_ptr<Thread> submitThread,
			smarter::shared_ptr<VirtualizedPageSpace> space,
			uintptr_t address, size_t length, const void *buffer,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		// Enter the submitter's work-queue so that we can access memory directly.
		co_await submitThread->mainWorkQueue()->schedule();

		enableUserAccess();
		auto error = space->store(address, length, buffer);
		disableUserAccess();
		assert(error == Error::success || error == Error::fault);

		HelSimpleResult helResult{.error = translateError(error), .reserved = {}};
		QueueSource ipcSource{&helResult, sizeof(HelSimpleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	};

	if(descriptor.is<MemoryViewDescriptor>()) {
		auto view = descriptor.get<MemoryViewDescriptor>().memory;
		writeMemoryView(thisThread.lock(),
				std::move(view), address, length, buffer, std::move(queue), context);
	}else if(descriptor.is<AddressSpaceDescriptor>()) {
		auto space = descriptor.get<AddressSpaceDescriptor>().space;
		writeAddressSpace(thisThread.lock(),
				std::move(space), address, length, buffer, std::move(queue), context);
	}else if(descriptor.is<ThreadDescriptor>()) {
		auto thread = descriptor.get<ThreadDescriptor>().thread;
		auto space = thread->getAddressSpace().lock();
		writeAddressSpace(thisThread.lock(),
				std::move(space), address, length, buffer, std::move(queue), context);
	}else if(descriptor.is<VirtualizedSpaceDescriptor>()) {
		auto space = descriptor.get<VirtualizedSpaceDescriptor>().space;
		writeVirtualizedSpace(thisThread.lock(),
				std::move(space), address, length, buffer, std::move(queue), context);
	}else{
		return kHelErrBadDescriptor;
	}

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	smarter::shared_ptr<MemoryView> memory;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	[](smarter::shared_ptr<IpcQueue> queue,
			smarter::shared_ptr<MemoryView> memory,
			uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		auto [error, type, offset, size] = co_await memory->submitManage();

		int helType;
		switch (type) {
			case ManageRequest::initialize: helType = kHelManageInitialize; break;
			case ManageRequest::writeback: helType = kHelManageWriteback; break;
			default:
				assert(!"unexpected ManageRequest");
				__builtin_trap();
		}

		HelManageResult helResult{translateError(error),
				helType, offset, size};
		QueueSource ipcSource{&helResult, sizeof(HelManageResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(queue), std::move(memory), context);

	return kHelErrNone;
}

HelError helUpdateMemory(HelHandle handle, int type,
		uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	if(error == Error::illegalObject)
		return kHelErrUnsupportedOperation;
	else if(error == Error::illegalArgs)
		return kHelErrIllegalArgs;

	assert(error == Error::success);
	return kHelErrNone;
}

HelError helSubmitLockMemoryView(HelHandle handle, uintptr_t offset, size_t size,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<MemoryView> memory;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	[](smarter::borrowed_ptr<thor::Universe> universe,
			smarter::shared_ptr<MemoryView> memory,
			smarter::shared_ptr<IpcQueue> queue,
			uintptr_t offset, size_t size,
			uintptr_t context, smarter::shared_ptr<WorkQueue> wq,
			enable_detached_coroutine = {}) -> void {
		MemoryViewLockHandle lockHandle{memory, offset, size};
		co_await lockHandle.acquire(wq);
		if(!lockHandle) {
			// TODO: Return a better error.
			HelHandleResult helResult{kHelErrFault, 0, 0};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}

		// Touch the memory range.
		// TODO: this should be optional (it is only really useful for no-backing mappings).
		auto touchOutcome = co_await memory->touchRange(offset, size, 0, wq);
		if(!touchOutcome) {
			HelHandleResult helResult{translateError(touchOutcome.error()), 0, kHelNullHandle};
			QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
			co_await queue->submit(&ipcSource, context);
			co_return;
		}

		// Attach the descriptor.
		HelHandle handle;
		{
			auto irq_lock = frg::guard(&irqMutex());
			Universe::Guard lock(universe->lock);

			handle = universe->attachDescriptor(lock,
					MemoryViewLockDescriptor{
						smarter::allocate_shared<NamedMemoryViewLock>(
							*kernelAlloc, std::move(lockHandle))});
		}

		HelHandleResult helResult{kHelErrNone, 0, handle};
		QueueSource ipcSource{&helResult, sizeof(HelHandleResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(this_universe), std::move(memory), std::move(queue),
		offset, size, context, this_thread->mainWorkQueue()->take());

	return kHelErrNone;
}

HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<MemoryView> memory;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryViewDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryViewDescriptor>().memory;
	}

/*	auto handle_load = smarter::allocate_shared<AsyncInitiateLoad>(*kernelAlloc,
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

	smarter::shared_ptr<Universe> universe;
	smarter::shared_ptr<AddressSpace, BindableHandle> space;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		if(universe_handle == kHelNullHandle) {
			universe = this_thread->getUniverse().lock();
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
	new_thread->self = remove_tag_cast(new_thread);

	// Adding a large prime (coprime to getCpuCount()) should yield a good distribution.
	auto cpu = globalNextCpu.fetch_add(4099, std::memory_order_relaxed) % getCpuCount();
//	infoLogger() << "thor: New thread on CPU #" << cpu << frg::endlog;
	Scheduler::associate(new_thread.get(), &getCpuData(cpu)->scheduler);
//	Scheduler::associate(new_thread.get(), localScheduler());
	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(remove_tag_cast(new_thread));

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(std::move(new_thread)));
	}

	return kHelErrNone;
}

HelError helQueryThreadStats(HelHandle handle, HelThreadStats *user_stats) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.lock();
	}else{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
	}

	HelThreadStats stats;
	memset(&stats, 0, sizeof(HelThreadStats));
	stats.userTime = thread->runTime();

	if(!writeUserObject(user_stats, stats))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helSetPriority(HelHandle handle, int priority) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.lock();
	}else{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
	}

	Scheduler::setPriority(thread.get(), priority);

	return kHelErrNone;
}

HelError helYield() {
	Thread::deferCurrent();

	return kHelErrNone;
}

HelError helSubmitObserve(HelHandle handle, uint64_t inSeq,
		HelHandle queueHandle, uintptr_t context) {
	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irqLock = frg::guard(&irqMutex());
		Universe::Guard universeGuard(thisUniverse->lock);

		auto threadWrapper = thisUniverse->getDescriptor(universeGuard, handle);
		if(!threadWrapper)
			return kHelErrNoDescriptor;
		if(!threadWrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = remove_tag_cast(threadWrapper->get<ThreadDescriptor>().thread);

		auto queueWrapper = thisUniverse->getDescriptor(universeGuard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	if(!queue->validSize(ipcSourceSize(sizeof(HelObserveResult))))
		return kHelErrQueueTooSmall;

	[] (smarter::shared_ptr<Thread> thread, uint64_t inSeq,
			smarter::shared_ptr<IpcQueue> queue, uintptr_t context,
			enable_detached_coroutine = {}) -> void {
		auto [error, sequence, interrupt] = co_await thread->observe(inSeq);

		HelObserveResult helResult{translateError(error), 0, sequence};
		if(interrupt == kIntrNull) {
			helResult.observation = kHelObserveNull;
		}else if(interrupt == kIntrDivByZero) {
			helResult.observation = kHelObserveDivByZero;
		}else if(interrupt == kIntrRequested) {
			helResult.observation = kHelObserveInterrupt;
		}else if(interrupt == kIntrPanic) {
			helResult.observation = kHelObservePanic;
		}else if(interrupt == kIntrBreakpoint) {
			helResult.observation = kHelObserveBreakpoint;
		}else if(interrupt == kIntrPageFault) {
			helResult.observation = kHelObservePageFault;
		}else if(interrupt == kIntrGeneralFault) {
			helResult.observation = kHelObserveGeneralFault;
		}else if(interrupt == kIntrIllegalInstruction) {
			helResult.observation = kHelObserveIllegalInstruction;
		}else if(interrupt >= kIntrSuperCall) {
			helResult.observation = kHelObserveSuperCall + (interrupt - kIntrSuperCall);
		}else{
			thor::panicLogger() << "Unexpected interrupt" << frg::endlog;
			__builtin_unreachable();
		}
		QueueSource ipcSource{&helResult, sizeof(HelObserveResult), nullptr};
		co_await queue->submit(&ipcSource, context);
	}(std::move(thread), inSeq, std::move(queue), context);
	return kHelErrNone;
}

HelError helKillThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
	}

	Thread::killOther(thread);

	return kHelErrNone;
}

HelError helInterruptThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
	}

	Thread::interruptOther(thread);

	return kHelErrNone;
}

HelError helResume(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
	}

	if(auto e = Thread::resumeOther(thread); e != Error::success) {
		if(e == Error::threadExited)
			return kHelErrThreadTerminated;
		assert(e == Error::illegalState);
		return kHelErrIllegalState;
	}

	return kHelErrNone;
}

HelError helLoadRegisters(HelHandle handle, int set, void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	VirtualizedCpuDescriptor vcpu;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(thread_wrapper->is<ThreadDescriptor>()) {
			thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
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
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2))
			return kHelErrFault;
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
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
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 15))
			return kHelErrFault;
#elif defined(__aarch64__)
		uintptr_t regs[31];
		for (int i = 0; i < 31; i++)
			regs[i] = thread->_executor.general()->x[i];
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 31))
			return kHelErrFault;
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsThread) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[2];
		regs[0] = thread->_executor.general()->clientFs;
		regs[1] = thread->_executor.general()->clientGs;
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2))
			return kHelErrFault;
#elif defined(__aarch64__)
		uintptr_t regs[1];
		regs[0] = thread->_executor.general()->tpidr_el0;
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 1))
			return kHelErrFault;
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsVirtualization) {
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		HelX86VirtualizationRegs regs;
		memset(&regs, 0, sizeof(HelX86VirtualizationRegs));
		vcpu.vcpu->loadRegs(&regs);
		if(!writeUserObject(reinterpret_cast<HelX86VirtualizationRegs *>(image), regs))
			return kHelErrFault;
#else
		return kHelErrNoHardwareSupport;
#endif
	}else if(set == kHelRegsSimd) {
#if defined(__x86_64__)
		if(!writeUserMemory(image, thread->_executor._fxState(), Executor::determineSimdSize()))
			return kHelErrFault;
#elif defined(__aarch64__)
		if(!writeUserMemory(image, &thread->_executor.general()->fp, sizeof(FpRegisters)))
			return kHelErrFault;
#endif
	}else if(set == kHelRegsSignal) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[19];
		regs[0] = thread->_executor.general()->r8;
		regs[1] = thread->_executor.general()->r9;
		regs[2] = thread->_executor.general()->r10;
		regs[3] = thread->_executor.general()->r11;
		regs[4] = thread->_executor.general()->r12;
		regs[5] = thread->_executor.general()->r13;
		regs[6] = thread->_executor.general()->r14;
		regs[7] = thread->_executor.general()->r15;
		regs[8] = thread->_executor.general()->rdi;
		regs[9] = thread->_executor.general()->rsi;
		regs[10] = thread->_executor.general()->rbp;
		regs[11] = thread->_executor.general()->rbx;
		regs[12] = thread->_executor.general()->rdx;
		regs[13] = thread->_executor.general()->rax;
		regs[14] = thread->_executor.general()->rcx;
		regs[15] = thread->_executor.general()->rsp;
		regs[16] = thread->_executor.general()->rip;
		regs[17] = thread->_executor.general()->rflags;
		regs[18] = thread->_executor.general()->cs;
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 19))
			return kHelErrFault;
#elif defined(__aarch64__)
		uintptr_t regs[35];
		regs[0] = thread->_executor.general()->far;
		for (int i = 0; i < 31; i++)
			regs[1 + i] = thread->_executor.general()->x[i];
		regs[32] = thread->_executor.general()->sp;
		regs[33] = thread->_executor.general()->elr;
		regs[34] = thread->_executor.general()->spsr;
		if(!writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 35))
			return kHelErrFault;
#else
		return kHelErrUnsupportedOperation;
#endif
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<Thread> thread;
	VirtualizedCpuDescriptor vcpu{0};
	if(handle == kHelThisThread) {
		// FIXME: Properly handle this below.
		thread = this_thread.lock();
	}else{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(thread_wrapper->is<ThreadDescriptor>()) {
			thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
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
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2))
			return kHelErrFault;
		*thread->_executor.ip() = regs[0];
		*thread->_executor.sp() = regs[1];
	}else if(set == kHelRegsGeneral) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		uintptr_t regs[15];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 15))
			return kHelErrFault;
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
#elif defined(__aarch64__)
		uintptr_t regs[31];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 31))
			return kHelErrFault;
		for (int i = 0; i < 31; i++)
			thread->_executor.general()->x[i] = regs[i];
#else
		return kHelErrUnsupportedOperation;
#endif
	}else if(set == kHelRegsThread) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#if defined(__x86_64__)
		uintptr_t regs[2];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2))
			return kHelErrFault;
		thread->_executor.general()->clientFs = regs[0];
		thread->_executor.general()->clientGs = regs[1];
#elif defined(__aarch64__)
		uintptr_t regs[1];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 1))
			return kHelErrFault;
		thread->_executor.general()->tpidr_el0 = regs[0];
#else
		return kHelErrUnsupportedOperation;

#endif
	}else if(set == kHelRegsDebug) {
#ifdef __x86_64__
		// FIXME: Make those registers thread-specific.
		uint32_t *reg;
		readUserObject(reinterpret_cast<uint32_t *const *>(image), reg);
		breakOnWrite(reg);
#endif
	}else if(set == kHelRegsVirtualization) {
#ifdef __x86_64__
		if(!vcpu.vcpu) {
			return kHelErrIllegalArgs;
		}
		HelX86VirtualizationRegs regs;
		if(!readUserObject(reinterpret_cast<const HelX86VirtualizationRegs *>(image), regs))
			return kHelErrFault;
		vcpu.vcpu->storeRegs(&regs);
#else
		return kHelErrNoHardwareSupport;
#endif
	}else if(set == kHelRegsSimd) {
#if defined(__x86_64__)
		if(!readUserMemory(thread->_executor._fxState(), image, Executor::determineSimdSize()))
			return kHelErrFault;
#elif defined(__aarch64__)
		if(!readUserMemory(&thread->_executor.general()->fp, image, sizeof(FpRegisters)))
			return kHelErrFault;
#endif
	}else if(set == kHelRegsSignal) {
		if(!thread) {
			return kHelErrIllegalArgs;
		}
#ifdef __x86_64__
		uintptr_t regs[19];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 19))
			return kHelErrFault;
		thread->_executor.general()->r8 = regs[0];
		thread->_executor.general()->r9 = regs[1];
		thread->_executor.general()->r10 = regs[2];
		thread->_executor.general()->r11 = regs[3];
		thread->_executor.general()->r12 = regs[4];
		thread->_executor.general()->r13 = regs[5];
		thread->_executor.general()->r14 = regs[6];
		thread->_executor.general()->r15 = regs[7];
		thread->_executor.general()->rdi = regs[8];
		thread->_executor.general()->rsi = regs[9];
		thread->_executor.general()->rbp = regs[10];
		thread->_executor.general()->rbx = regs[11];
		thread->_executor.general()->rdx = regs[12];
		thread->_executor.general()->rax = regs[13];
		thread->_executor.general()->rcx = regs[14];
		thread->_executor.general()->rsp = regs[15];
		thread->_executor.general()->rip = regs[16];

		// Allow modifying the normal non-privileged flags.
		constexpr uintptr_t allowedFlagsMask = 0b1000011000110111111111;
		thread->_executor.general()->rflags &= ~allowedFlagsMask;
		thread->_executor.general()->rflags |= regs[17] & allowedFlagsMask;

		// Make sure that the cs is in usermode by or'ing it with 3.
		thread->_executor.general()->cs = regs[18] | 3;
#elif defined(__aarch64__)
		uintptr_t regs[35];
		if(!readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 35))
			return kHelErrFault;
		thread->_executor.general()->far = regs[0];
		for (int i = 0; i < 31; i++)
			thread->_executor.general()->x[i] = regs[1 + i];
		thread->_executor.general()->sp = regs[32];
		thread->_executor.general()->elr = regs[33];

		// Allow N, Z, C, V and SS modifications.
		constexpr uintptr_t allowedFlagsMask = 0b1111U << 28 | 1U << 21;
		thread->_executor.general()->spsr &= ~allowedFlagsMask;
		thread->_executor.general()->spsr |= regs[34] & allowedFlagsMask;
#else
		return kHelErrUnsupportedOperation;
#endif
	}else {
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helWriteFsBase(void *pointer) {
#ifdef __x86_64__
	common::x86::wrmsr(common::x86::kMsrIndexFsBase, (uintptr_t)pointer);
	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helReadFsBase(void **pointer) {
#ifdef __x86_64__
	*pointer = (void *)common::x86::rdmsr(common::x86::kMsrIndexFsBase);
	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helWriteGsBase(void *pointer) {
#ifdef __x86_64__
	common::x86::wrmsr(common::x86::kMsrIndexKernelGsBase, (uintptr_t)pointer);
	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helReadGsBase(void **pointer) {
#ifdef __x86_64__
	*pointer = (void *)common::x86::rdmsr(common::x86::kMsrIndexKernelGsBase);
	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helGetClock(uint64_t *counter) {
	*counter = systemClockSource()->currentNanos();
	return kHelErrNone;
}

HelError helSubmitAwaitClock(uint64_t counter, HelHandle queue_handle, uintptr_t context,
		uint64_t *async_id) {
	struct Closure final : CancelNode, PrecisionTimerNode, IpcNode {
		static void issue(uint64_t nanos, smarter::shared_ptr<IpcQueue> queue,
				uintptr_t context, uint64_t *async_id) {
			auto closure = frg::construct<Closure>(*kernelAlloc, nanos,
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

		explicit Closure(uint64_t nanos, smarter::shared_ptr<IpcQueue> the_queue,
				uintptr_t context)
		: queue{std::move(the_queue)},
				source{&result, sizeof(HelSimpleResult), nullptr},
				result{translateError(Error::success), 0} {
			setupContext(context);
			setupSource(&source);

			worklet.setup(&Closure::elapsed, getCurrentThread()->mainWorkQueue());
			PrecisionTimerNode::setup(nanos, cancelEvent, &worklet);
		}

		void handleCancellation() override {
			cancelEvent.cancel();
		}

		void complete() override {
			frg::destruct(*kernelAlloc, this);
		}

		Worklet worklet;
		async::cancellation_event cancelEvent;
		smarter::shared_ptr<IpcQueue> queue;
		QueueSource source;
		HelSimpleResult result;
	};

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*lane1_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(std::move(lanes.get<0>())));
		*lane2_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(std::move(lanes.get<1>())));
	}

	return kHelErrNone;
}

HelError helSubmitAsync(HelHandle handle, const HelAction *actions, size_t count,
		HelHandle queueHandle, uintptr_t context, uint32_t flags) {
	if(flags)
		return kHelErrIllegalArgs;
	if(!count)
		return kHelErrIllegalArgs;

	auto thisThread = getCurrentThread();
	auto thisUniverse = thisThread->getUniverse();

	LaneHandle lane;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(thisUniverse->lock);

		auto wrapper = thisUniverse->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(wrapper->is<LaneDescriptor>()) {
			lane = wrapper->get<LaneDescriptor>().handle;
		}else{
			return kHelErrBadDescriptor;
		}

		auto queueWrapper = thisUniverse->getDescriptor(universe_guard, queueHandle);
		if(!queueWrapper)
			return kHelErrNoDescriptor;
		if(!queueWrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queueWrapper->get<QueueDescriptor>().queue;
	}

	struct Item {
		HelAction recipe;
		size_t link;
		StreamNode transmit;
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

	frg::dyn_array<Item, KernelAlloc> items{count, *kernelAlloc};

	// Identifies the root chain on the stack below.
	constexpr size_t noIndex = static_cast<size_t>(-1);

	// Auxiliary stack to compute the list that each item will be linked into.
	frg::small_vector<size_t, 4, KernelAlloc> linkStack{*kernelAlloc};
	linkStack.push_back(noIndex);

	// Read the message items from userspace.
	size_t ipcSize = 0;
	size_t numFlows = 0;
	for(size_t i = 0; i < count; i++) {
		HelAction *recipe = &items[i].recipe;
		auto node = &items[i].transmit;

		readUserObject(actions + i, *recipe);

		switch(recipe->type) {
			case kHelActionDismiss:
				node->_tag = kTagDismiss;
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			case kHelActionOffer:
				node->_tag = kTagOffer;
				ipcSize += ipcSourceSize(sizeof(HelHandleResult));
				break;
			case kHelActionAccept:
				node->_tag = kTagAccept;
				ipcSize += ipcSourceSize(sizeof(HelHandleResult));
				break;
			case kHelActionImbueCredentials: {
				smarter::shared_ptr<Credentials> creds;

				if(recipe->handle == kHelThisThread) {
					creds = thisThread.lock();
				} else {
					auto irq_lock = frg::guard(&irqMutex());
					Universe::Guard universe_guard(thisUniverse->lock);

					auto wrapper = thisUniverse->getDescriptor(universe_guard, recipe->handle);
					if(!wrapper) {
						return kHelErrNoDescriptor;
					}
					if(wrapper->is<ThreadDescriptor>())
						creds = remove_tag_cast(wrapper->get<ThreadDescriptor>().thread);
					else if(wrapper->is<LaneDescriptor>())
						creds = wrapper->get<LaneDescriptor>().handle.getStream().lock();
					else
						return kHelErrBadDescriptor;
				}

				node->_tag = kTagImbueCredentials;
				memcpy(node->_inCredentials.data(), creds->credentials(), 16);
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			}
			case kHelActionExtractCredentials:
				node->_tag = kTagExtractCredentials;
				ipcSize += ipcSourceSize(sizeof(HelCredentialsResult));
				break;
			case kHelActionSendFromBuffer:
				if(recipe->length <= kPageSize) {
					frg::unique_memory<KernelAlloc> buffer(*kernelAlloc, recipe->length);
					if(!readUserMemory(reinterpret_cast<char *>(buffer.data()),
							reinterpret_cast<char *>(recipe->buffer), recipe->length))
						return kHelErrFault;

					node->_tag = kTagSendKernelBuffer;
					node->_inBuffer = std::move(buffer);
				}else{
					node->_tag = kTagSendFlow;
					node->_maxLength = recipe->length;
					++numFlows;
				}
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			case kHelActionSendFromBufferSg: {
				size_t length = 0;
				auto sglist = reinterpret_cast<HelSgItem *>(recipe->buffer);
				for(size_t j = 0; j < recipe->length; j++) {
					HelSgItem item;
					readUserObject(sglist + j, item);
					length += item.length;
				}

				frg::unique_memory<KernelAlloc> buffer(*kernelAlloc, length);
				size_t offset = 0;
				for(size_t j = 0; j < recipe->length; j++) {
					HelSgItem item;
					readUserObject(sglist + j, item);
					if(!readUserMemory(reinterpret_cast<char *>(buffer.data()) + offset,
							reinterpret_cast<char *>(item.buffer), item.length))
						return kHelErrFault;
					offset += item.length;
				}

				node->_tag = kTagSendKernelBuffer;
				node->_inBuffer = std::move(buffer);
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			}
			case kHelActionRecvInline:
				// TODO: For now, we hardcode a size of 128 bytes.
				node->_tag = kTagRecvKernelBuffer;
				node->_maxLength = 128;
				ipcSize += ipcSourceSize(sizeof(HelLengthResult));
				ipcSize += ipcSourceSize(128);
				break;
			case kHelActionRecvToBuffer:
				node->_tag = kTagRecvFlow;
				node->_maxLength = recipe->length;
				++numFlows;
				ipcSize += ipcSourceSize(sizeof(HelLengthResult));
				break;
			case kHelActionPushDescriptor: {
				AnyDescriptor operand;
				{
					auto irq_lock = frg::guard(&irqMutex());
					Universe::Guard universe_guard(thisUniverse->lock);

					auto wrapper = thisUniverse->getDescriptor(universe_guard, recipe->handle);
					if(!wrapper)
						return kHelErrNoDescriptor;
					operand = *wrapper;
				}

				node->_tag = kTagPushDescriptor;
				node->_inDescriptor = std::move(operand);
				ipcSize += ipcSourceSize(sizeof(HelSimpleResult));
				break;
			}
			case kHelActionPullDescriptor:
				node->_tag = kTagPullDescriptor;
				ipcSize += ipcSourceSize(sizeof(HelHandleResult));
				break;
			default:
				return kHelErrIllegalArgs;
		}

		// Items at the root must be chained.
		if(linkStack.empty())
			return kHelErrIllegalArgs;

		items[i].link = linkStack.back();

		if(!(recipe->flags & kHelItemChain))
			linkStack.pop_back();
		if(recipe->flags & kHelItemAncillary)
			linkStack.push_back(i);
	}

	// All chains must terminate properly.
	if(!linkStack.empty())
		return kHelErrIllegalArgs;

	if(!queue->validSize(ipcSize))
		return kHelErrQueueTooSmall;

	// From this point on, the function must not fail, since we now link our items
	// into intrusive linked lists.

	struct Closure final : StreamPacket, IpcNode {
		static void transmitted(Closure *closure) {
			QueueSource *tail = nullptr;
			auto link = [&] (QueueSource *source) {
				if(tail)
					tail->link = source;
				tail = source;
			};

			for(size_t i = 0; i < closure->count; i++) {
				auto item = &closure->items[i];
				HelAction *recipe = &item->recipe;
				auto node = &item->transmit;

				if(recipe->type == kHelActionDismiss) {
					item->helSimpleResult = {translateError(node->error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionOffer) {
					HelHandle handle = kHelNullHandle;

					if(node->error() == Error::success
							&& (recipe->flags & kHelItemWantLane)) {
						auto universe = closure->weakUniverse.lock();
						if (!universe) {
							item->helHandleResult = {kHelErrBadDescriptor, 0, handle};
							item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
							link(&item->mainSource);
							continue;
						}
						assert(universe);

						auto irq_lock = frg::guard(&irqMutex());
						Universe::Guard lock(universe->lock);

						handle = universe->attachDescriptor(lock,
								LaneDescriptor{node->lane()});
					}

					item->helHandleResult = {translateError(node->error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionAccept) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(node->error() == Error::success) {
						auto universe = closure->weakUniverse.lock();
						assert(universe);

						auto irq_lock = frg::guard(&irqMutex());
						Universe::Guard lock(universe->lock);

						handle = universe->attachDescriptor(lock,
								LaneDescriptor{node->lane()});
					}

					item->helHandleResult = {translateError(node->error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionImbueCredentials) {
					item->helSimpleResult = {translateError(node->error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionExtractCredentials) {
					item->helCredentialsResult = {.error = translateError(node->error()), .reserved = {}, .credentials = {}};
					memcpy(item->helCredentialsResult.credentials,
							node->credentials().data(), 16);
					item->mainSource.setup(&item->helCredentialsResult,
							sizeof(HelCredentialsResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionSendFromBuffer
						|| recipe->type == kHelActionSendFromBufferSg) {
					item->helSimpleResult = {translateError(node->error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionRecvInline) {
					item->helInlineResult = {translateError(node->error()),
							0, node->_transmitBuffer.size()};
					item->mainSource.setup(&item->helInlineResult, sizeof(HelInlineResultNoFlex));
					item->dataSource.setup(node->_transmitBuffer.data(),
							node->_transmitBuffer.size());
					link(&item->mainSource);
					link(&item->dataSource);
				}else if(recipe->type == kHelActionRecvToBuffer) {
					item->helLengthResult = {translateError(node->error()),
							0, node->actualLength()};
					item->mainSource.setup(&item->helLengthResult, sizeof(HelLengthResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionPushDescriptor) {
					item->helSimpleResult = {translateError(node->error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(recipe->type == kHelActionPullDescriptor) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(node->error() == Error::success) {
						auto universe = closure->weakUniverse.lock();
						assert(universe);

						auto irq_lock = frg::guard(&irqMutex());
						Universe::Guard lock(universe->lock);

						handle = universe->attachDescriptor(lock, node->descriptor());
					}

					item->helHandleResult = {translateError(node->error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else{
					// This cannot happen since we validate recipes at submit time.
					__builtin_trap();
				}
			}

			closure->setupSource(&closure->items[0].mainSource);
			closure->ipcQueue->submit(closure);
		}

		Closure(frg::dyn_array<Item, KernelAlloc> items_)
		: items{std::move(items_)} { }

		void completePacket() override {
			transmitted(this);
		}

		void complete() override {
			frg::destruct(*kernelAlloc, this);
		}

		size_t count;
		smarter::weak_ptr<Universe> weakUniverse;
		smarter::shared_ptr<IpcQueue> ipcQueue;
		frg::dyn_array<Item, KernelAlloc> items;
	} *closure = frg::construct<Closure>(*kernelAlloc, std::move(items));

	closure->count = count;
	closure->weakUniverse = thisUniverse.lock();
	closure->ipcQueue = std::move(queue);

	closure->setup(count);
	closure->setupContext(context);

	// Now, build up the messages that we submit to the stream.
	StreamList rootChain;
	for(size_t i = 0; i < count; i++) {
		// Setup the packet pointer.
		closure->items[i].transmit._packet = closure;

		// Link the nodes together.
		auto l = closure->items[i].link;
		if(l == noIndex) {
			rootChain.push_back(&closure->items[i].transmit);
		}else{
			// Add the item to an ancillary list of another item.
			closure->items[l].transmit.ancillaryChain.push_back(&closure->items[i].transmit);
		}
	}

	auto handleFlow = [] (Closure *closure, size_t numFlows,
			smarter::shared_ptr<Thread> thread,
			enable_detached_coroutine = {}) -> void {
		// We exit once we processed numFlows-many items.
		// This guarantees that we do not access the closure object after it is freed.
		// Below, we need to ensure that we always complete our own nodes
		// before completing peer nodes.

		// The size of this array must be a power of two.
		frg::array<frg::unique_memory<KernelAlloc>, 2> xferBuffers;

		size_t i = 0;
		size_t seenFlows = 0; // Iterates through flows.
		while(seenFlows < numFlows) {
			assert(i < closure->count);
			auto item = &closure->items[i++];
			auto recipe = &item->recipe;
			auto node = &item->transmit;

			if(!usesFlowProtocol(node->tag()))
				continue;
			++seenFlows;

			co_await node->issueFlow.wait();
			auto peer = node->peerNode;

			// Check for transmission errors (transmission errors or zero-size transfers).
			if(!peer) {
				node->complete();
				continue;
			}

			if(recipe->type == kHelActionSendFromBuffer
					&& node->tag() == kTagSendFlow
					&& peer->tag() == kTagRecvKernelBuffer) {
				frg::unique_memory<KernelAlloc> buffer(*kernelAlloc, recipe->length);

				co_await thread->mainWorkQueue()->enter();
				auto outcome = readUserMemory(buffer.data(), recipe->buffer, recipe->length);
				if(!outcome) {
					// We complete with fault; the remote with success.
					// TODO: it probably makes sense to introduce a "remote fault" error.
					peer->_error = Error::success;
					node->_error = Error::fault;
					peer->complete();
					node->complete();
					continue;
				}

				// Both nodes complete successfully.
				peer->_transmitBuffer = std::move(buffer);
				peer->complete();
				node->complete();
			}else if(recipe->type == kHelActionSendFromBuffer
					&& node->tag() == kTagSendFlow
					&& peer->tag() == kTagRecvFlow) {
				// Empty packets are handled by the generic stream code.
				assert(recipe->length);

				size_t progress = 0;
				size_t numSent = 0;
				size_t numAcked = 0;
				bool lastTransferSent = false;
				// Each iteration of this loop sends one transfer packet (or terminates).
				while(true) {
					bool anyRemoteFault = false;
					while(numSent != numAcked) {
						// If there is anything more to send, we only need to wait until
						// at least one buffer is not in-flight (otherwise, we wait for all).
						if(!lastTransferSent && numSent - numAcked < xferBuffers.size())
							break;
						auto ackPacket = co_await node->flowQueue.async_get();
						assert(ackPacket);
						if(ackPacket->fault)
							anyRemoteFault = true;
						++numAcked;
					}

					if(lastTransferSent) {
						if(anyRemoteFault) {
							node->_error = Error::remoteFault;
						}else{
							node->_error = Error::success;
						}
						break;
					}

					// If we encounter remote faults, we terminate.
					if(anyRemoteFault) {
						// Send the packet (may deallocate the peer!).
						peer->flowQueue.put({ .terminate = true });
						++numSent;

						// Retrieve but ignore all acks.
						assert(numSent > numAcked);
						while(numSent != numAcked) {
							auto ackPacket = co_await node->flowQueue.async_get();
							assert(ackPacket);
							++numAcked;
						}

						node->_error = Error::remoteFault;
						break;
					}

					// Prepare a buffer an send it.
					assert(numSent - numAcked < xferBuffers.size());
					auto &xb = xferBuffers[numSent & (xferBuffers.size() - 1)];
					if(!xb.size())
						xb = frg::unique_memory<KernelAlloc>{*kernelAlloc, 4096};

					auto chunkSize = frg::min(recipe->length - progress, xb.size());
					assert(chunkSize);

					co_await thread->mainWorkQueue()->enter();
					auto outcome = readUserMemory(xb.data(),
							reinterpret_cast<std::byte *>(recipe->buffer) + progress, chunkSize);
					if(!outcome) {
						// Send the packet (may deallocate the peer!).
						peer->flowQueue.put({ .terminate = true, .fault = true });
						++numSent;

						// Retrieve but ignore all acks.
						assert(numSent > numAcked);
						while(numSent != numAcked) {
							auto ackPacket = co_await node->flowQueue.async_get();
							assert(ackPacket);
							++numAcked;
						}

						node->_error = Error::fault;
						break;
					}

					lastTransferSent = (progress + chunkSize == recipe->length);
					// Send the packet (may deallocate the peer!).
					peer->flowQueue.put({
						.data = xb.data(),
						.size = chunkSize,
						.terminate = lastTransferSent
					});
					++numSent;
					progress += chunkSize;
				}

				node->complete();
			}else if(recipe->type == kHelActionRecvToBuffer
					&& peer->tag() == kTagSendKernelBuffer) {
				co_await thread->mainWorkQueue()->enter();
				auto outcome = writeUserMemory(recipe->buffer,
						peer->_inBuffer.data(), peer->_inBuffer.size());
				if(!outcome) {
					// We complete with fault; the remote with success.
					// TODO: it probably makes sense to introduce a "remote fault" error.
					peer->_error = Error::success;
					node->_error = Error::fault;
					peer->complete();
					node->complete();
					continue;
				}

				// Both nodes complete successfully.
				node->_actualLength = peer->_inBuffer.size();
				peer->complete();
				node->complete();
			}else{
				assert(recipe->type == kHelActionRecvToBuffer
						&& peer->tag() == kTagSendFlow);

				size_t progress = 0;
				bool didFault = false;
				// Each iteration of this loop sends one ack packet.
				while(true) {
					auto xferPacket = co_await node->flowQueue.async_get();
					assert(xferPacket);

					if(xferPacket->data && !didFault) {
						// Otherwise, there would have been a transmission error.
						assert(progress + xferPacket->size <= recipe->length);

						co_await thread->mainWorkQueue()->enter();
						auto outcome = writeUserMemory(
								reinterpret_cast<std::byte *>(recipe->buffer) + progress,
								xferPacket->data, xferPacket->size);
						if(outcome) {
							progress += xferPacket->size;
						}else{
							didFault = true;
						}
					}

					if(xferPacket->terminate) {
						if(didFault) {
							// Ack the packet (may deallocate the peer!).
							peer->flowQueue.put({ .terminate = true, .fault = true, });
							node->_error = Error::fault;
						}else{
							// Ack the packet (may deallocate the peer!).
							peer->flowQueue.put({ .terminate = true });
							if(xferPacket->fault) {
								node->_error = Error::remoteFault;
							}else{
								node->_actualLength = progress;
							}
						}

						// This node is finished.
						break;
					}

					// This is the only non-terminating case.
					// Senders should always terminate if they fault.
					assert(!xferPacket->fault);

					// Ack the packet (may deallocate the peer!).
					if(didFault) {
						peer->flowQueue.put({ .fault = true });
					}else{
						peer->flowQueue.put({});
					}
				}

				node->complete();
			}
		}
	};

	if(numFlows)
		handleFlow(closure, numFlows, thisThread.lock());

	Stream::transmit(lane, rootChain);

	return kHelErrNone;
}

HelError helShutdownLane(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	LaneHandle lane;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

HelError helFutexWait(int *pointer, int expected, int64_t deadline) {
	auto thisThread = getCurrentThread();
	auto space = thisThread->getAddressSpace();

	auto futexOrError = Thread::asyncBlockCurrent(
			space->grabGlobalFutex(reinterpret_cast<uintptr_t>(pointer),
					thisThread->mainWorkQueue()->take()));
	if(!futexOrError)
		return kHelErrFault;
	GlobalFutex futex = std::move(futexOrError.value());

	if(deadline < 0) {
		if(deadline != -1)
			return kHelErrIllegalArgs;

		Thread::asyncBlockCurrent(
			getGlobalFutexRealm()->wait(std::move(futex), expected)
		);
	}else{
		Thread::asyncBlockCurrent(
			async::race_and_cancel(
				[&] (async::cancellation_token cancellation) {
					return getGlobalFutexRealm()->wait(std::move(futex), expected,
							cancellation);
				},
				[&] (async::cancellation_token cancellation) {
					return generalTimerEngine()->sleep(deadline, cancellation);
				}
			)
		);
	}

	return kHelErrNone;
}

HelError helFutexWake(int *pointer) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	auto identityOrError = space->resolveGlobalFutex(reinterpret_cast<uintptr_t>(pointer));
	if(!identityOrError)
		return kHelErrFault;
	getGlobalFutexRealm()->wake(identityOrError.value());

	return kHelErrNone;
}

HelError helCreateOneshotEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = smarter::allocate_shared<OneshotEvent>(*kernelAlloc);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				OneshotEventDescriptor(std::move(event)));
	}

	return kHelErrNone;
}

HelError helCreateBitsetEvent(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto event = smarter::allocate_shared<BitsetEvent>(*kernelAlloc);

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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
#ifdef __x86_64__
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq = smarter::allocate_shared<GenericIrqObject>(*kernelAlloc,
			frg::string<KernelAlloc>{*kernelAlloc, "generic-irq-object"});
	IrqPin::attachSink(getGlobalSystemIrq(number), irq.get());

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IrqDescriptor(std::move(irq)));
	}

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence) {
	if(flags & ~(kHelAckAcknowledge | kHelAckNack | kHelAckKick | kHelAckClear))
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto mode = flags & (kHelAckAcknowledge | kHelAckNack | kHelAckKick);
	if(mode != kHelAckAcknowledge && mode != kHelAckNack && mode != kHelAckKick)
		return kHelErrIllegalArgs;

	smarter::shared_ptr<IrqObject> irq;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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
		error = IrqPin::kickSink(irq.get(), flags & kHelAckClear);
	}

	if(error == Error::illegalArgs) {
		return kHelErrIllegalArgs;
	}else{
		assert(error == Error::success);
		return kHelErrNone;
	}
}

HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		HelHandle queue_handle, uintptr_t context) {
	struct IrqClosure final : IpcNode {
		static void issue(smarter::shared_ptr<IrqObject> irq, uint64_t sequence,
				smarter::shared_ptr<IpcQueue> queue, intptr_t context) {
			auto closure = frg::construct<IrqClosure>(*kernelAlloc,
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
		explicit IrqClosure(smarter::shared_ptr<IpcQueue> the_queue, uintptr_t context)
		: _queue{std::move(the_queue)},
				source{&result, sizeof(HelEventResult), nullptr} {
			memset(&result, 0, sizeof(HelEventResult));
			setupContext(context);
			setupSource(&source);
			worklet.setup(&IrqClosure::awaited, getCurrentThread()->mainWorkQueue());
			irqNode.setup(&worklet);
		}

		void complete() override {
			frg::destruct(*kernelAlloc, this);
		}

	private:
		Worklet worklet;
		AwaitIrqNode irqNode;
		smarter::shared_ptr<IpcQueue> _queue;
		QueueSource source;
		HelEventResult result;
	};

	struct EventClosure final : IpcNode {
		static void issue(smarter::shared_ptr<OneshotEvent> event, uint64_t sequence,
				smarter::shared_ptr<IpcQueue> queue, intptr_t context) {
			auto closure = frg::construct<EventClosure>(*kernelAlloc,
					std::move(queue), context);
			event->submitAwait(&closure->eventNode, sequence);
		}

		static void issue(smarter::shared_ptr<BitsetEvent> event, uint64_t sequence,
				smarter::shared_ptr<IpcQueue> queue, intptr_t context) {
			auto closure = frg::construct<EventClosure>(*kernelAlloc,
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
		explicit EventClosure(smarter::shared_ptr<IpcQueue> the_queue, uintptr_t context)
		: _queue{std::move(the_queue)},
				source{&result, sizeof(HelEventResult), nullptr} {
			memset(&result, 0, sizeof(HelEventResult));
			setupContext(context);
			setupSource(&source);
			worklet.setup(&EventClosure::awaited, getCurrentThread()->mainWorkQueue());
			eventNode.setup(&worklet);
		}

		void complete() override {
			frg::destruct(*kernelAlloc, this);
		}

	private:
		Worklet worklet;
		AwaitEventNode eventNode;
		smarter::shared_ptr<IpcQueue> _queue;
		QueueSource source;
		HelEventResult result;
	};

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<IrqObject> irq;
	AnyDescriptor descriptor;
	smarter::shared_ptr<IpcQueue> queue;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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

	smarter::shared_ptr<IrqObject> irq;
	smarter::shared_ptr<BoundKernlet> kernlet;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

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
	auto io_space = smarter::allocate_shared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++) {
		uintptr_t port;
		readUserObject<uintptr_t>(port_array + i, port);
		io_space->addPort(port);
	}

	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IoDescriptor(std::move(io_space)));
	}

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
#ifdef THOR_ARCH_SUPPORTS_PIO
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<IoSpace> io_space;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<IoDescriptor>())
			return kHelErrBadDescriptor;
		io_space = wrapper->get<IoDescriptor>().ioSpace;
	}

	io_space->enableInThread(this_thread);

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helEnableFullIo() {
#ifdef THOR_ARCH_SUPPORTS_PIO
	auto this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->getContext().enableIoPort(port);

	return kHelErrNone;
#else
	return kHelErrUnsupportedOperation;
#endif
}

HelError helBindKernlet(HelHandle handle, const HelKernletData *data, size_t num_data,
		HelHandle *bound_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::shared_ptr<KernletObject> kernlet;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto kernlet_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!kernlet_wrapper)
			return kHelErrNoDescriptor;
		if(!kernlet_wrapper->is<KernletObjectDescriptor>())
			return kHelErrBadDescriptor;
		kernlet = kernlet_wrapper->get<KernletObjectDescriptor>().kernletObject;
	}

	auto object = kernlet.get();
	assert(num_data == object->numberOfBindParameters());

	auto bound = smarter::allocate_shared<BoundKernlet>(*kernelAlloc,
			std::move(kernlet));
	for(size_t i = 0; i < object->numberOfBindParameters(); i++) {
		const auto &defn = object->defnOfBindParameter(i);

		HelKernletData d;
		if(!readUserObject(data + i, d))
			return kHelErrFault;

		if(defn.type == KernletParameterType::offset) {
			bound->setupOffsetBinding(i, d.handle);
		}else if(defn.type == KernletParameterType::memoryView) {
			smarter::shared_ptr<MemoryView> memory;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard universe_guard(this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, d.handle);
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

			smarter::shared_ptr<BitsetEvent> event;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard universe_guard(this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, d.handle);
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
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		*bound_handle = this_universe->attachDescriptor(universe_guard,
				BoundKernletDescriptor(std::move(bound)));
	}

	return kHelErrNone;
}

HelError helGetAffinity(HelHandle handle, uint8_t *mask, size_t size, size_t *actualSize) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	smarter::borrowed_ptr<Thread> thread;
	{
		auto irq_lock = frg::guard(&irqMutex());
		Universe::Guard universe_guard(this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
	}

	frg::vector<uint8_t, KernelAlloc> buf = thread->getAffinityMask();

	if(buf.size() > size)
		return kHelErrBufferTooSmall;

	size_t used_size = size > buf.size() ? buf.size() : size;

	if (!writeUserArray(mask, buf.data(), used_size))
		return kHelErrFault;

	if (actualSize != nullptr)
		if (!writeUserObject<size_t>(actualSize, used_size))
			return kHelErrFault;

	return kHelErrNone;
}

HelError helSetAffinity(HelHandle handle, uint8_t *mask, size_t size) {
	frg::vector<uint8_t, KernelAlloc> buf{*kernelAlloc};
	buf.resize(size);

	if (!readUserArray(mask, buf.data(), size))
		return kHelErrFault;

	size_t n = 0;
	for (auto i : buf) {
		n += __builtin_popcount(i);
	}

	if (n < 1) {
		return kHelErrIllegalArgs;
	}

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	if(handle == kHelThisThread) {
		this_thread->setAffinityMask(std::move(buf));
		Thread::migrateCurrent();
	} else {
		smarter::borrowed_ptr<Thread> thread;
		{
			auto irq_lock = frg::guard(&irqMutex());
			Universe::Guard universe_guard(this_universe->lock);

			auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
			if(!thread_wrapper)
				return kHelErrNoDescriptor;
			if(!thread_wrapper->is<ThreadDescriptor>())
				return kHelErrBadDescriptor;
			thread = remove_tag_cast(thread_wrapper->get<ThreadDescriptor>().thread);
		}

		thread->setAffinityMask(std::move(buf));
		infoLogger() << "thor: TODO: helSetAffinity does not migrate other threads!" << frg::endlog;
	}

	return kHelErrNone;
}

HelError helQueryRegisterInfo(int set, HelRegisterInfo *info) {
	HelRegisterInfo outInfo;

	switch (set) {
		case kHelRegsProgram:
			outInfo.setSize = 2 * sizeof(uintptr_t);
			break;

		case kHelRegsGeneral:
#if defined (__x86_64__)
			outInfo.setSize = 15 * sizeof(uintptr_t);
#elif defined (__aarch64__)
			outInfo.setSize = 31 * sizeof(uintptr_t);
#else
#			error Unknown architecture
#endif
			break;

		case kHelRegsThread:
#if defined (__x86_64__)
			outInfo.setSize = 2 * sizeof(uintptr_t);
#elif defined (__aarch64__)
			outInfo.setSize = 1 * sizeof(uintptr_t);
#else
#			error Unknown architecture
#endif
			break;

#if defined (__x86_64__)
		case kHelRegsVirtualization:
			outInfo.setSize = sizeof(HelX86VirtualizationRegs);
			break;
#endif

		case kHelRegsSimd:
#if defined (__x86_64__)
			outInfo.setSize = Executor::determineSimdSize();
#elif defined (__aarch64__)
			outInfo.setSize = sizeof(FpRegisters);
#else
#			error Unknown architecture
#endif
			break;

		case kHelRegsSignal:
#if defined (__x86_64__)
			outInfo.setSize = 19 * sizeof(uintptr_t);
#elif defined (__aarch64__)
			outInfo.setSize = 35 * sizeof(uintptr_t);
#else
#			error Unknown architecture
#endif
			break;

		default:
			return kHelErrIllegalArgs;
	}

	if (!writeUserObject(info, outInfo))
		return kHelErrFault;

	return kHelErrNone;
}

HelError helGetCurrentCpu(int *cpu) {
	*cpu = getCpuData()->cpuIndex;
	return kHelErrNone;
}
