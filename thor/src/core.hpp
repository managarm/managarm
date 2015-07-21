
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

// --------------------------------------------------------
// Memory related classes
// --------------------------------------------------------

class Memory : public SharedObject {
public:
	Memory();

	void resize(size_t length);

	uintptr_t getPage(int index);

private:
	util::Vector<uintptr_t, KernelAlloc> p_physicalPages;
};

// --------------------------------------------------------
// Event-related classes
// --------------------------------------------------------

struct SubmitInfo {
	SubmitInfo(int64_t submit_id, uintptr_t submit_function,
			uintptr_t submit_object);
	
	int64_t submitId;
	uintptr_t submitFunction;
	uintptr_t submitObject;
};

class EventHub : public SharedObject {
public:
	struct Event {
		enum Type {
			kTypeNone,
			kTypeRecvStringTransfer,
			kTypeRecvStringError,
			kTypeIrq
		};

		Event(Type type, SubmitInfo submit_info);
		
		Type type;
		Error error;
		SubmitInfo submitInfo;
		uint8_t *kernelBuffer;
		uint8_t *userBuffer;
		size_t length;
	};

	EventHub();

	void raiseRecvStringTransferEvent(uint8_t *kernel_buffer,
			uint8_t *user_buffer, size_t length,
			SubmitInfo submit_info);
	void raiseRecvStringErrorEvent(Error error,
			SubmitInfo submit_info);
	void raiseIrqEvent(SubmitInfo submit_info);

	bool hasEvent();
	Event dequeueEvent();

private:
	util::LinkedList<Event, KernelAlloc> p_queue;
};

// --------------------------------------------------------
// IPC-related classes
// --------------------------------------------------------

// Single producer, single consumer connection
class Channel {
public:
	struct Message {
	public:
		Message(uint8_t *kernel_buffer, size_t length,
			int64_t msg_request, int64_t msg_sequence);

		uint8_t *kernelBuffer;
		size_t length;
		int64_t msgRequest;
		int64_t msgSequence;
	};

	Channel();

	void sendString(const uint8_t *buffer, size_t length,
			int64_t msg_request, int64_t msg_sequence);
	void submitRecvString(SharedPtr<EventHub> &&event_hub,
			uint8_t *user_buffer, size_t length,
			int64_t filter_request, int64_t filter_sequence,
			SubmitInfo submit_info);

private:
	struct Request {
		Request(SharedPtr<EventHub> &&event_hub,
				int64_t filter_request, int64_t filter_sequence,
				SubmitInfo submit_info);

		SharedPtr<EventHub> eventHub;
		SubmitInfo submitInfo;
		uint8_t *userBuffer;
		size_t maxLength;
		int64_t filterRequest;
		int64_t filterSequence;
	};

	bool matchRequest(const Message &message, const Request &request);

	// returns true if the message + request are consumed
	bool processStringRequest(const Message &message, const Request &request);

	util::LinkedList<Message, KernelAlloc> p_messages;
	util::LinkedList<Request, KernelAlloc> p_requests;
};

class BiDirectionPipe : public SharedObject {
public:
	BiDirectionPipe();

	Channel *getFirstChannel();
	Channel *getSecondChannel();

private:
	Channel p_firstChannel;
	Channel p_secondChannel;
};

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

class Thread : public SharedObject {
friend class ThreadQueue;
public:
	void setup(void (*user_entry)(uintptr_t), uintptr_t argument,
			void *user_stack_ptr);
	
	UnsafePtr<Universe> getUniverse();
	UnsafePtr<AddressSpace> getAddressSpace();
	
	void setUniverse(SharedPtr<Universe> &&universe);
	void setAddressSpace(SharedPtr<AddressSpace> &&address_space);

	void enableIoPort(uintptr_t port);
	
	void switchTo();

private:
	SharedPtr<Universe> p_universe;
	SharedPtr<AddressSpace> p_addressSpace;

	SharedPtr<Thread> p_nextInQueue;
	UnsafePtr<Thread> p_previousInQueue;

	ThorRtThreadState p_state;
	frigg::arch_x86::Tss64 p_tss;
};

class ThreadQueue {
public:
	bool empty();

	void addBack(SharedPtr<Thread> &&thread);
	
	SharedPtr<Thread> removeFront();
	SharedPtr<Thread> remove(UnsafePtr<Thread> thread);

private:
	SharedPtr<Thread> p_front;
	UnsafePtr<Thread> p_back;
};

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

class MemoryAccessDescriptor {
public:
	MemoryAccessDescriptor(SharedPtr<Memory> &&memory);

	UnsafePtr<Memory> getMemory();

private:
	SharedPtr<Memory> p_memory;
};


class ThreadObserveDescriptor {
public:
	ThreadObserveDescriptor(SharedPtr<Thread> &&thread);
	
	UnsafePtr<Thread> getThread();

private:
	SharedPtr<Thread> p_thread;
};


class EventHubDescriptor {
public:
	EventHubDescriptor(SharedPtr<EventHub> &&event_hub);

	UnsafePtr<EventHub> getEventHub();

private:
	SharedPtr<EventHub> p_eventHub;
};


// Reads from the first channel, writes to the second
class BiDirectionFirstDescriptor {
public:
	BiDirectionFirstDescriptor(SharedPtr<BiDirectionPipe> &&pipe);
	
	UnsafePtr<BiDirectionPipe> getPipe();

private:
	SharedPtr<BiDirectionPipe> p_pipe;
};

// Reads from the second channel, writes to the first
class BiDirectionSecondDescriptor {
public:
	BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe> &&pipe);
	
	UnsafePtr<BiDirectionPipe> getPipe();

private:
	SharedPtr<BiDirectionPipe> p_pipe;
};


class IrqDescriptor {
public:
	IrqDescriptor(SharedPtr<IrqLine> &&thread);
	
	UnsafePtr<IrqLine> getIrqLine();

private:
	SharedPtr<IrqLine> p_irqLine;
};

class IoDescriptor {
public:
	IoDescriptor(SharedPtr<IoSpace> &&thread);
	
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
	util::Hashmap<Handle, AnyDescriptor, util::DefaultHasher<Handle>, KernelAlloc> p_descriptorMap;
	Handle p_nextHandle;
};

class AddressSpace : public SharedObject {
public:
	AddressSpace(memory::PageSpace page_space);

	void mapSingle4k(void *address, uintptr_t physical);

private:
	memory::PageSpace p_pageSpace;
};


extern LazyInitializer<SharedPtr<Thread>> currentThread;

} // namespace thor

void *operator new(size_t length, thor::KernelAlloc *);

