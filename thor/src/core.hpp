
namespace thor {

// --------------------------------------------------------
// Debugging and logging
// --------------------------------------------------------

class BochsSink {
public:
	void print(char c);
	void print(const char *str);
};

extern BochsSink infoSink;
extern LazyInitializer<frigg::debug::DefaultLogger<BochsSink>> infoLogger;

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

class KernelVirtualAlloc {
public:
	KernelVirtualAlloc();

	uintptr_t map(size_t length);
	void unmap(uintptr_t address, size_t length);

private:
	uintptr_t p_nextPage;
};

typedef frigg::memory::DebugAllocator<KernelVirtualAlloc> KernelAlloc;
extern LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
extern LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
extern LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// Kernel data types
// --------------------------------------------------------

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

struct CpuContext {
	SharedPtr<Thread, KernelAlloc> currentThread;
};

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

	frigg::util::LinkedList<Request, KernelAlloc> p_requests;
};

extern LazyInitializer<IrqRelay[16]> irqRelays;

class IrqLine {
public:
	IrqLine(int number);

	int getNumber();

private:
	int p_number;
};

class IoSpace {
public:
	IoSpace();

	void addPort(uintptr_t port);

	void enableInThread(UnsafePtr<Thread, KernelAlloc> thread);

private:
	frigg::util::Vector<uintptr_t, KernelAlloc> p_ports;
};

// --------------------------------------------------------
// Process related classes
// --------------------------------------------------------

class Universe {
public:
	Universe();
	
	Handle attachDescriptor(AnyDescriptor &&descriptor);

	AnyDescriptor &getDescriptor(Handle handle);
	
	AnyDescriptor detachDescriptor(Handle handle);

private:
	frigg::util::Hashmap<Handle, AnyDescriptor,
			frigg::util::DefaultHasher<Handle>, KernelAlloc> p_descriptorMap;
	Handle p_nextHandle;
};


UnsafePtr<Thread, KernelAlloc> getCurrentThread();

} // namespace thor

