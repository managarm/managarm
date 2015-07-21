
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

void EventHub::raiseRecvStringErrorEvent(Error error,
		SubmitInfo submit_info) {
	Event event(Event::kTypeRecvStringError, submit_info);
	event.error = error;
	p_queue.addBack(util::move(event));
}

void EventHub::raiseRecvStringTransferEvent(uint8_t *kernel_buffer,
		uint8_t *user_buffer, size_t length, SubmitInfo submit_info) {
	Event event(Event::kTypeRecvStringTransfer, submit_info);
	event.kernelBuffer = kernel_buffer;
	event.userBuffer = user_buffer;
	event.length = length;
	p_queue.addBack(util::move(event));
}

void EventHub::raiseAcceptEvent(SharedPtr<BiDirectionPipe> &&pipe,
		SubmitInfo submit_info) {
	Event event(Event::kTypeAccept, submit_info);
	event.pipe = util::move(pipe);
	p_queue.addBack(util::move(event));
}

void EventHub::raiseConnectEvent(SharedPtr<BiDirectionPipe> &&pipe,
		SubmitInfo submit_info) {
	Event event(Event::kTypeConnect, submit_info);
	event.pipe = util::move(pipe);
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

Channel::Channel() : p_messages(kernelAlloc.get()),
		p_requests(kernelAlloc.get()) { }

void Channel::sendString(const uint8_t *user_buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence) {
	uint8_t *buffer = (uint8_t *)kernelAlloc->allocate(length);
	for(size_t i = 0; i < length; i++)
		buffer[i] = user_buffer[i];
	
	Message message(buffer, length, msg_request, msg_sequence);
	
	bool queue_message = true;
	for(auto it = p_requests.frontIter(); it.okay(); ++it) {
		if(!matchRequest(message, *it))
			continue;
		
		if(processStringRequest(message, *it)) {
			p_requests.remove(it);
			// don't queue the message if a request succeeds
			queue_message = false;
			break;
		}
	}

	if(queue_message)
		p_messages.addBack(util::move(message));
}

void Channel::submitRecvString(SharedPtr<EventHub> &&event_hub,
		uint8_t *user_buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info) {
	Request request(util::move(event_hub),
			filter_request, filter_sequence, submit_info);
	request.userBuffer = user_buffer;
	request.maxLength = max_length;

	bool queue_request = true;
	for(auto it = p_messages.frontIter(); it.okay(); ++it) {
		if(!matchRequest(*it, request))
			continue;
		
		if(processStringRequest(*it, request))
			p_messages.remove(it);
		// NOTE: we never queue failed requests
		queue_request = false;
		break;
	}
	
	if(queue_request)
		p_requests.addBack(util::move(request));
}

bool Channel::matchRequest(const Message &message, const Request &request) {
	if(request.filterRequest != -1)
		if(request.filterRequest != message.msgRequest)
			return false;
	
	if(request.filterSequence != -1)
		if(request.filterSequence != message.msgSequence)
			return false;
	
	return true;
}

bool Channel::processStringRequest(const Message &message, const Request &request) {
	if(message.length > request.maxLength) {
		request.eventHub->raiseRecvStringErrorEvent(kErrBufferTooSmall,
				request.submitInfo);
		return false;
	}else{
		request.eventHub->raiseRecvStringTransferEvent(message.kernelBuffer,
				request.userBuffer, message.length, request.submitInfo);
		return true;
	}
}


Channel::Message::Message(uint8_t *buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence)
	: kernelBuffer(buffer), length(length),
		msgRequest(msg_request), msgSequence(msg_sequence) { }


Channel::Request::Request(SharedPtr<EventHub> &&event_hub,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info),
		userBuffer(nullptr), maxLength(0),
		filterRequest(filter_request), filterSequence(filter_sequence) { }


BiDirectionPipe::BiDirectionPipe() {

}

Channel *BiDirectionPipe::getFirstChannel() {
	return &p_firstChannel;
}

Channel *BiDirectionPipe::getSecondChannel() {
	return &p_secondChannel;
}


Server::Server() : p_acceptRequests(kernelAlloc.get()),
		p_connectRequests(kernelAlloc.get()) { }

void Server::submitAccept(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	AcceptRequest request(util::move(event_hub), submit_info);
	
	if(!p_connectRequests.empty()) {
		processRequests(request, p_connectRequests.front());
		p_connectRequests.removeFront();
	}else{
		p_acceptRequests.addBack(util::move(request));
	}
}

void Server::submitConnect(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	ConnectRequest request(util::move(event_hub), submit_info);

	if(!p_acceptRequests.empty()) {
		processRequests(p_acceptRequests.front(), request);
		p_acceptRequests.removeFront();
	}else{
		p_connectRequests.addBack(util::move(request));
	}
}

void Server::processRequests(const AcceptRequest &accept,
		const ConnectRequest &connect) {
	auto pipe = makeShared<BiDirectionPipe>(kernelAlloc.get());

	accept.eventHub->raiseAcceptEvent(pipe->shared<BiDirectionPipe>(),
			accept.submitInfo);
	connect.eventHub->raiseConnectEvent(pipe->shared<BiDirectionPipe>(),
			connect.submitInfo);
}

Server::AcceptRequest::AcceptRequest(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info) { }

Server::ConnectRequest::ConnectRequest(SharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info) { }

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

UnsafePtr<BiDirectionPipe> BiDirectionFirstDescriptor::getPipe() {
	return p_pipe->unsafe<BiDirectionPipe>();
}


BiDirectionSecondDescriptor::BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe> &&pipe)
		: p_pipe(util::move(pipe)) { }

UnsafePtr<BiDirectionPipe> BiDirectionSecondDescriptor::getPipe() {
	return p_pipe->unsafe<BiDirectionPipe>();
}


ServerDescriptor::ServerDescriptor(SharedPtr<Server> &&server)
		: p_server(util::move(server)) { }

UnsafePtr<Server> ServerDescriptor::getServer() {
	return p_server->unsafe<Server>();
}

ClientDescriptor::ClientDescriptor(SharedPtr<Server> &&server)
		: p_server(util::move(server)) { }

UnsafePtr<Server> ClientDescriptor::getServer() {
	return p_server->unsafe<Server>();
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

