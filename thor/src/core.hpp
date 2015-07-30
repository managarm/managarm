
#include "util/hashmap.hpp"
#include "util/linked.hpp"
#include "util/variant.hpp"

namespace thor {

typedef memory::StupidMemoryAllocator KernelAlloc;

extern LazyInitializer<KernelAlloc> kernelAlloc;

extern void *kernelStackBase;
extern size_t kernelStackLength;

enum Error {
	kErrSuccess,
	kErrBufferTooSmall
};

typedef uint64_t Handle;

class Universe;
class Memory;
class AddressSpace;
class Thread;
class EventHub;
class Channel;
class BiDirectionPipe;
class Server;
class RdFolder;
class IrqLine;
class IoSpace;

} // namespace thor

#include "descriptor.hpp"
#include "usermem.hpp"
#include "event.hpp"
#include "ipc.hpp"
#include "thread.hpp"
#include "rd.hpp"

namespace thor {

// --------------------------------------------------------
// I/O related functions
// --------------------------------------------------------

class IrqRelay {
public:
	IrqRelay();

	void submitWaitRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
			SubmitInfo submit_info);
	
	void fire();

private:
	struct Request {
		Request(SharedPtr<EventHub, KernelAlloc> &&event_hub,
				SubmitInfo submit_info);

		SharedPtr<EventHub, KernelAlloc> eventHub;
		SubmitInfo submitInfo;
	};

	util::LinkedList<Request, KernelAlloc> p_requests;
};

extern LazyInitializer<IrqRelay[16]> irqRelays;

class IrqLine : public SharedBase<IrqLine, KernelAlloc> {
public:
	IrqLine(int number);

	int getNumber();

private:
	int p_number;
};

class IoSpace : public SharedBase<IoSpace, KernelAlloc> {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(UnsafePtr<Thread, KernelAlloc> thread);

private:
	util::Vector<uintptr_t, KernelAlloc> p_ports;
};

// --------------------------------------------------------
// Process related classes
// --------------------------------------------------------

class Universe : public SharedBase<Universe, KernelAlloc> {
public:
	Universe();
	
	Handle attachDescriptor(AnyDescriptor &&descriptor);

	AnyDescriptor &getDescriptor(Handle handle);
	
	AnyDescriptor detachDescriptor(Handle handle);

private:
	util::Hashmap<Handle, AnyDescriptor,
			util::DefaultHasher<Handle>, KernelAlloc> p_descriptorMap;
	Handle p_nextHandle;
};


extern LazyInitializer<SharedPtr<Thread, KernelAlloc>> currentThread;

} // namespace thor

