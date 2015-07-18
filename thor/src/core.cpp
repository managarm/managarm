
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"

namespace thor {

LazyInitializer<SharedPtr<Thread>> currentThread;

LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// Memory related classes
// --------------------------------------------------------

Memory::Memory()
		: p_physicalPages(kernelAlloc.get()) { }

void Memory::resize(size_t length) {
	for(size_t l = 0; l < length; l += 0x1000) {
		uintptr_t page = memory::tableAllocator->allocate(1);
		p_physicalPages.push(page);
	}
}

uintptr_t Memory::getPage(int index) {
	return p_physicalPages[index];
}

// --------------------------------------------------------
// Event related classes
// --------------------------------------------------------

SubmitInfo::SubmitInfo(int64_t submit_id,
		uintptr_t submit_function, uintptr_t submit_object)
	: submitId(submit_id), submitFunction(submit_function),
		submitObject(submit_object) { }

EventHub::Event::Event(Type type, SubmitInfo submit_info)
		: type(type), submitInfo(submit_info) { }

EventHub::EventHub() : p_queue(kernelAlloc.get()) { }

void EventHub::raiseIrqEvent(SubmitInfo submit_info) {
	Event event(Event::kTypeIrq, submit_info);
	p_queue.addBack(util::move(event));
}

bool EventHub::hasEvent() {
	return !p_queue.empty();
}

EventHub::Event EventHub::dequeueEvent() {
	return p_queue.removeFront();
}

// --------------------------------------------------------
// IPC related classes
// --------------------------------------------------------

Channel::Channel() : p_messages(kernelAlloc.get()) { }

void Channel::recvString(char *user_buffer, size_t length) {
	auto &message = p_messages[0];

	for(size_t i = 0; i < message.getLength(); i++)
		user_buffer[i] = message.getBuffer()[i];
}

void Channel::sendString(const char *user_buffer, size_t length) {
	char *buffer = (char *)kernelAlloc->allocate(length);
	for(size_t i = 0; i < length; i++)
		buffer[i] = user_buffer[i];
	
	p_messages.push(Message(buffer, length));
}


Channel::Message::Message(char *buffer, size_t length)
		: p_buffer(buffer), p_length(length) { }

char *Channel::Message::getBuffer() {
	return p_buffer;
}

size_t Channel::Message::getLength() {
	return p_length;
}


BiDirectionPipe::BiDirectionPipe() {

}

Channel *BiDirectionPipe::getFirstChannel() {
	return &p_firstChannel;
}

Channel *BiDirectionPipe::getSecondChannel() {
	return &p_secondChannel;
}

// --------------------------------------------------------
// Descriptors
// --------------------------------------------------------

MemoryAccessDescriptor::MemoryAccessDescriptor(SharedPtr<Memory> &&memory)
		: p_memory(util::move(memory)) { }

UnsafePtr<Memory> MemoryAccessDescriptor::getMemory() {
	return p_memory->unsafe<Memory>();
}


EventHubDescriptor::EventHubDescriptor(SharedPtr<EventHub> &&event_hub)
		: p_eventHub(util::move(event_hub)) { }

UnsafePtr<EventHub> EventHubDescriptor::getEventHub() {
	return p_eventHub->unsafe<EventHub>();
}


BiDirectionFirstDescriptor::BiDirectionFirstDescriptor(SharedPtr<BiDirectionPipe> &&pipe)
		: p_pipe(util::move(pipe)) { }

void BiDirectionFirstDescriptor::recvString(char *buffer, size_t length) {
	p_pipe->getFirstChannel()->recvString(buffer, length);
}

void BiDirectionFirstDescriptor::sendString(const char *buffer, size_t length) {
	p_pipe->getSecondChannel()->sendString(buffer, length);
}


BiDirectionSecondDescriptor::BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe> &&pipe)
		: p_pipe(util::move(pipe)) { }

void BiDirectionSecondDescriptor::recvString(char *buffer, size_t length) {
	p_pipe->getSecondChannel()->recvString(buffer, length);
}

void BiDirectionSecondDescriptor::sendString(const char *buffer, size_t length) {
	p_pipe->getFirstChannel()->sendString(buffer, length);
}


IrqDescriptor::IrqDescriptor(SharedPtr<IrqLine> &&irq_line)
		: p_irqLine(util::move(irq_line)) { }

UnsafePtr<IrqLine> IrqDescriptor::getIrqLine() {
	return p_irqLine->unsafe<IrqLine>();
}

IoDescriptor::IoDescriptor(SharedPtr<IoSpace> &&io_space)
		: p_ioSpace(util::move(io_space)) { }

UnsafePtr<IoSpace> IoDescriptor::getIoSpace() {
	return p_ioSpace->unsafe<IoSpace>();
}

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

AnyDescriptor::AnyDescriptor(MemoryAccessDescriptor &&descriptor)
		: p_type(kTypeMemoryAccess), p_memoryAccessDescriptor(util::move(descriptor)) { }

AnyDescriptor::AnyDescriptor(EventHubDescriptor &&descriptor)
		: p_type(kTypeEventHub), p_eventHubDescriptor(util::move(descriptor)) { }

AnyDescriptor::AnyDescriptor(BiDirectionFirstDescriptor &&descriptor)
		: p_type(kTypeBiDirectionFirst), p_biDirectionFirstDescriptor(util::move(descriptor)) { }

AnyDescriptor::AnyDescriptor(BiDirectionSecondDescriptor &&descriptor)
		: p_type(kTypeBiDirectionSecond), p_biDirectionSecondDescriptor(util::move(descriptor)) { }

AnyDescriptor::AnyDescriptor(IrqDescriptor &&descriptor)
		: p_type(kTypeIrq), p_irqDescriptor(util::move(descriptor)) { }

AnyDescriptor::AnyDescriptor(IoDescriptor &&descriptor)
		: p_type(kTypeIo), p_ioDescriptor(util::move(descriptor)) { }

AnyDescriptor::AnyDescriptor(AnyDescriptor &&other) : p_type(other.p_type) {
	switch(p_type) {
	case kTypeMemoryAccess:
		new (&p_memoryAccessDescriptor) MemoryAccessDescriptor(util::move(other.p_memoryAccessDescriptor));
		break;
	case kTypeEventHub:
		new (&p_eventHubDescriptor) EventHubDescriptor(util::move(other.p_eventHubDescriptor));
		break;
	case kTypeBiDirectionFirst:
		new (&p_biDirectionFirstDescriptor) BiDirectionFirstDescriptor(util::move(other.p_biDirectionFirstDescriptor));
		break;
	case kTypeBiDirectionSecond:
		new (&p_biDirectionSecondDescriptor) BiDirectionSecondDescriptor(util::move(other.p_biDirectionSecondDescriptor));
		break;
	case kTypeIrq:
		new (&p_irqDescriptor) IrqDescriptor(util::move(other.p_irqDescriptor));
		break;
	case kTypeIo:
		new (&p_ioDescriptor) IoDescriptor(util::move(other.p_ioDescriptor));
		break;
	default:
		debug::criticalLogger->log("Illegal descriptor");
		debug::panic();
	}
}
AnyDescriptor &AnyDescriptor::operator= (AnyDescriptor &&other) {
	p_type = other.p_type;
	switch(p_type) {
	case kTypeMemoryAccess:
		p_memoryAccessDescriptor = util::move(other.p_memoryAccessDescriptor);
		break;
	case kTypeEventHub:
		p_eventHubDescriptor = util::move(other.p_eventHubDescriptor);
		break;
	case kTypeBiDirectionFirst:
		p_biDirectionFirstDescriptor = util::move(other.p_biDirectionFirstDescriptor);
		break;
	case kTypeBiDirectionSecond:
		p_biDirectionSecondDescriptor = util::move(other.p_biDirectionSecondDescriptor);
		break;
	case kTypeIrq:
		p_irqDescriptor = util::move(other.p_irqDescriptor);
		break;
	case kTypeIo:
		p_ioDescriptor = util::move(other.p_ioDescriptor);
		break;
	default:
		debug::criticalLogger->log("Illegal descriptor");
		debug::panic();
	}
}

auto AnyDescriptor::getType() -> Type {
	return p_type;
}

MemoryAccessDescriptor &AnyDescriptor::asMemoryAccess() {
	return p_memoryAccessDescriptor;
}
EventHubDescriptor &AnyDescriptor::asEventHub() {
	return p_eventHubDescriptor;
}
BiDirectionFirstDescriptor &AnyDescriptor::asBiDirectionFirst() {
	return p_biDirectionFirstDescriptor;
}
BiDirectionSecondDescriptor &AnyDescriptor::asBiDirectionSecond() {
	return p_biDirectionSecondDescriptor;
}
IrqDescriptor &AnyDescriptor::asIrq() {
	return p_irqDescriptor;
}
IoDescriptor &AnyDescriptor::asIo() {
	return p_ioDescriptor;
}

Universe::Universe()
		: p_descriptorMap(util::DefaultHasher<Handle>(), kernelAlloc.get()) { }

AnyDescriptor &Universe::getDescriptor(Handle handle) {
	return p_descriptorMap.get(handle);
}

// --------------------------------------------------------
// AddressSpace
// --------------------------------------------------------

AddressSpace::AddressSpace(memory::PageSpace page_space)
		: p_pageSpace(page_space) { }

void AddressSpace::mapSingle4k(void *address, uintptr_t physical) {
	p_pageSpace.mapSingle4k(address, physical);
}

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

const Word kRflagsBase = 0x1;
const Word kRflagsIf = 0x200;

void Thread::setup(void (*user_entry)(uintptr_t), uintptr_t argument,
		void *user_stack_ptr) {
	p_state.rflags = kRflagsBase | kRflagsIf;
	p_state.rdi = (Word)argument;
	p_state.rip = (Word)user_entry;
	p_state.rsp = (Word)user_stack_ptr;
	
	frigg::arch_x86::initializeTss64(&p_tss);
	p_tss.rsp0 = 0xFFFF800100200000;

}

UnsafePtr<Universe> Thread::getUniverse() {
	return p_universe->unsafe<Universe>();
}
UnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return p_addressSpace->unsafe<AddressSpace>();
}

void Thread::setUniverse(SharedPtr<Universe> &&universe) {
	p_universe = util::move(universe);
}
void Thread::setAddressSpace(SharedPtr<AddressSpace> &&address_space) {
	p_addressSpace = util::move(address_space);
}

void Thread::enableIoPort(uintptr_t port) {
	p_tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void Thread::switchTo() {
	UnsafePtr<Thread> previous_thread = (*currentThread)->unsafe<Thread>();
	*currentThread = this->shared<Thread>();
	
	thorRtUserContext = &p_state;
	thorRtEnableTss(&p_tss);
}


bool ThreadQueue::empty() {
	return p_front.get() == nullptr;
}

void ThreadQueue::addBack(SharedPtr<Thread> &&thread) {
	// setup the back pointer before moving the thread pointer
	UnsafePtr<Thread> back = p_back;
	p_back = thread->unsafe<Thread>();
	
	// move the thread pointer into the queue
	if(empty()) {
		p_front = util::move(thread);
	}else{
		thread->p_previousInQueue = back;
		back->p_nextInQueue = util::move(thread);
	}
}

SharedPtr<Thread> ThreadQueue::removeFront() {
	if(empty()) {
		debug::criticalLogger->log("ThreadQueue::removeFront(): List is empty!");
		debug::panic();
	}
	
	// move the front and second element out of the queue
	SharedPtr<Thread> front = util::move(p_front);
	SharedPtr<Thread> next = util::move(front->p_nextInQueue);
	front->p_previousInQueue = UnsafePtr<Thread>();

	// fix the pointers to previous elements
	if(next.get() == nullptr) {
		p_back = UnsafePtr<Thread>();
	}else{
		next->p_previousInQueue = UnsafePtr<Thread>();
	}

	// move the second element back to the queue
	p_front = util::move(next);

	return front;
}

SharedPtr<Thread> ThreadQueue::remove(UnsafePtr<Thread> thread) {
	// move the successor out of the queue
	SharedPtr<Thread> next = util::move(thread->p_nextInQueue);
	UnsafePtr<Thread> previous = thread->p_previousInQueue;
	thread->p_previousInQueue = UnsafePtr<Thread>();

	// fix pointers to previous elements
	if(p_back.get() == thread.get()) {
		p_back = previous;
	}else{
		next->p_previousInQueue = previous;
	}
	
	// move the successor back to the queue
	// move the thread out of the queue
	SharedPtr<Thread> reference;
	if(p_front.get() == thread.get()) {
		reference = util::move(p_front);
		p_front = util::move(next);
	}else{
		reference = util::move(previous->p_nextInQueue);
		previous->p_nextInQueue = util::move(next);
	}

	return reference;
}

// --------------------------------------------------------
// Io
// --------------------------------------------------------

IrqRelay::Request::Request(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info) { }

LazyInitializer<IrqRelay[16]> irqRelays;

IrqRelay::IrqRelay() : p_requests(kernelAlloc.get()) { }

void IrqRelay::submitWaitRequest(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	Request request(util::move(event_hub), submit_info);
	p_requests.addBack(util::move(request));
}

void IrqRelay::fire() {
	while(!p_requests.empty()) {
		Request request = p_requests.removeFront();
		request.eventHub->raiseIrqEvent(request.submitInfo);
	}
}


IrqLine::IrqLine(int number) : p_number(number) { }

int IrqLine::getNumber() {
	return p_number;
}


IoSpace::IoSpace() : p_ports(kernelAlloc.get()) { }

void IoSpace::addPort(uintptr_t port) {
	p_ports.push(port);
}

void IoSpace::enableInThread(UnsafePtr<Thread> thread) {
	for(size_t i = 0; i < p_ports.size(); i++)
		thread->enableIoPort(p_ports[i]);
}

} // namespace thor

void *operator new(size_t length, thor::KernelAlloc *allocator) {
	return allocator->allocate(length);
}

