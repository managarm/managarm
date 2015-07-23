
#include "util/hashmap.hpp"
#include "util/linked.hpp"
#include "util/variant.hpp"

namespace thor {

typedef memory::StupidMemoryAllocator KernelAlloc;

extern LazyInitializer<KernelAlloc> kernelAlloc;

enum Error {
	kErrSuccess,
	kErrBufferTooSmall
};

typedef uint64_t Handle;

class Universe;
class AddressSpace;
class Channel;
class BiDirectionPipe;

} // namespace thor

#include "usermem.hpp"
#include "event.hpp"
#include "ipc.hpp"
#include "thread.hpp"

namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IrqRelay {
public:
	IrqRelay();

	void submitWaitRequest(SharedPtr<EventHub> &&event_hub,
			SubmitInfo submit_info);
	
	void fire();

private:
	struct Request {
		Request(SharedPtr<EventHub> &&event_hub,
				SubmitInfo submit_info);

		SharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
	};

	util::LinkedList<Request, KernelAlloc> p_requests;
};

extern LazyInitializer<IrqRelay[16]> irqRelays;

class IrqLine : public SharedObject {
public:
	IrqLine(int number);

	int getNumber();

private:
	int p_number;
};

class IoSpace : public SharedObject {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(UnsafePtr<Thread> thread);

private:
	util::Vector<uintptr_t, KernelAlloc> p_ports;
};

// --------------------------------------------------------
// Descriptors
// --------------------------------------------------------

class ThreadObserveDescriptor {
public:
	ThreadObserveDescriptor(SharedPtr<Thread> &&thread);
	
	UnsafePtr<Thread> getThread();

private:
	SharedPtr<Thread> p_thread;
};


class IrqDescriptor {
public:
	IrqDescriptor(SharedPtr<IrqLine> &&irq_line);
	
	UnsafePtr<IrqLine> getIrqLine();

private:
	SharedPtr<IrqLine> p_irqLine;
};

class IoDescriptor {
public:
	IoDescriptor(SharedPtr<IoSpace> &&io_space);
	
	UnsafePtr<IoSpace> getIoSpace();

private:
	SharedPtr<IoSpace> p_ioSpace;
};

// --------------------------------------------------------
// Process related classes
// --------------------------------------------------------

typedef util::Variant<MemoryAccessDescriptor,
		ThreadObserveDescriptor,
		EventHubDescriptor,
		BiDirectionFirstDescriptor,
		BiDirectionSecondDescriptor,
		ServerDescriptor,
		ClientDescriptor,
		IrqDescriptor,
		IoDescriptor> AnyDescriptor;

class Universe : public SharedObject {
public:
	Universe();

	template<typename... Args>
	Handle attachDescriptor(Args &&... args) {
		Handle handle = p_nextHandle++;
		p_descriptorMap.insert(handle, AnyDescriptor(util::forward<Args>(args)...));
		return handle;
	}

	AnyDescriptor &getDescriptor(Handle handle);

private:
	util::Hashmap<Handle, AnyDescriptor,
			util::DefaultHasher<Handle>, KernelAlloc> p_descriptorMap;
	Handle p_nextHandle;
};


extern LazyInitializer<SharedPtr<Thread>> currentThread;

template<typename T, typename... Args>
T *construct(thor::KernelAlloc &allocator, Args &&... args) {
	void *pointer = allocator.allocate(sizeof(T));
	return new(pointer) T(util::forward<Args>(args)...);
}

template<typename T>
void destruct(thor::KernelAlloc &allocator, T *pointer) {
	allocator.free(pointer);
}

} // namespace thor

void *operator new(size_t length, thor::KernelAlloc *);

