
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Channel
// --------------------------------------------------------

Channel::Channel()
: p_messages(*kernelAlloc), p_requests(*kernelAlloc), p_wasClosed(false) { }

Error Channel::sendString(Guard &guard, const void *user_buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence, uint32_t flags) {
	assert(guard.protects(&lock));

	if(p_wasClosed)
		return kErrPipeClosed;

	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), user_buffer, length);
	
	AsyncSendString message(kMsgString, msg_request, msg_sequence);
	message.flags = flags;
	message.kernelBuffer = frigg::move(kernel_buffer);

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
		p_messages.addBack(frigg::move(message));
	return kErrSuccess;
}

Error Channel::sendDescriptor(Guard &guard, AnyDescriptor &&descriptor,
		int64_t msg_request, int64_t msg_sequence, uint32_t flags) {
	assert(guard.protects(&lock));

	if(p_wasClosed)
		return kErrPipeClosed;

	AsyncSendString message(kMsgDescriptor, msg_request, msg_sequence);
	message.flags = flags;
	message.descriptor = frigg::move(descriptor);

	for(auto it = p_requests.frontIter(); it.okay(); ++it) {
		if(!matchRequest(message, *it))
			continue;
		
		processDescriptorRequest(message, *it);
		p_requests.remove(it);
		return kErrSuccess;
	}

	p_messages.addBack(frigg::move(message));
	return kErrSuccess;
}

Error Channel::submitRecvString(Guard &guard, frigg::SharedPtr<EventHub> event_hub,
		ForeignSpaceLock space_lock,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info, uint32_t flags) {
	assert(guard.protects(&lock));

	if(p_wasClosed)
		return kErrPipeClosed;

	AsyncRecvString request(kMsgStringToBuffer, frigg::move(event_hub),
			filter_request, filter_sequence, submit_info);
	request.flags = flags;
	request.spaceLock = frigg::move(space_lock);

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
		p_requests.addBack(frigg::move(request));
	return kErrSuccess;
}

Error Channel::submitRecvStringToRing(Guard &guard, frigg::SharedPtr<EventHub> event_hub,
		frigg::SharedPtr<RingBuffer> ring_buffer,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info, uint32_t flags) {
	assert(guard.protects(&lock));

	if(p_wasClosed)
		return kErrPipeClosed;

	AsyncRecvString request(kMsgStringToRing, frigg::move(event_hub),
			filter_request, filter_sequence, submit_info);
	request.flags = flags;
	request.ringBuffer = frigg::move(ring_buffer);

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
		p_requests.addBack(frigg::move(request));
	return kErrSuccess;
}

Error Channel::submitRecvDescriptor(Guard &guard, frigg::SharedPtr<EventHub> event_hub,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info, uint32_t flags) {
	assert(guard.protects(&lock));

	if(p_wasClosed)
		return kErrPipeClosed;

	AsyncRecvString request(kMsgDescriptor, frigg::move(event_hub),
			filter_request, filter_sequence, submit_info);
	request.flags = flags;

	for(auto it = p_messages.frontIter(); it.okay(); ++it) {
		if(!matchRequest(*it, request))
			continue;
		
		processDescriptorRequest(*it, request);
		p_messages.remove(it);
		return kErrSuccess;
	}
	
	p_requests.addBack(frigg::move(request));
	return kErrSuccess;
}

void Channel::close(Guard &guard) {
	while(!p_messages.empty())
		p_messages.removeFront();

	while(!p_requests.empty()) {
		AsyncRecvString request = p_requests.removeFront();
		UserEvent event(UserEvent::kTypeError, request.submitInfo);
		event.error = kErrPipeClosed;

		EventHub::Guard hub_guard(&request.eventHub->lock);
		request.eventHub->raiseEvent(hub_guard, frigg::move(event));
		hub_guard.unlock();
	}

	p_wasClosed = true;
}

bool Channel::matchRequest(const AsyncSendString &message, const AsyncRecvString &request) {
	if(message.type == kMsgString) {
		if(request.type != kMsgStringToBuffer && request.type != kMsgStringToRing)
			return false;
	}else if(message.type != request.type) {
		return false;
	}

	if((bool)(request.flags & kFlagRequest) != (bool)(message.flags & kFlagRequest))
		return false;
	if((bool)(request.flags & kFlagResponse) != (bool)(message.flags & kFlagResponse))
		return false;
	
	if(request.filterRequest != -1)
		if(request.filterRequest != message.msgRequest)
			return false;
	
	if(request.filterSequence != -1)
		if(request.filterSequence != message.msgSequence)
			return false;
	
	return true;
}

bool Channel::processStringRequest(AsyncSendString &send, AsyncRecvString &recv) {
	if(recv.type == kMsgStringToBuffer) {
		if(send.kernelBuffer.size() <= recv.spaceLock.length()) {
			// perform the actual data transfer
			recv.spaceLock.copyTo(send.kernelBuffer.data(), send.kernelBuffer.size());

			// post the receive event
			UserEvent event(UserEvent::kTypeRecvStringTransferToBuffer, recv.submitInfo);
			event.length = send.kernelBuffer.size();
			event.msgRequest = send.msgRequest;
			event.msgSequence = send.msgSequence;
		
			EventHub::Guard recv_guard(&recv.eventHub->lock);
			recv.eventHub->raiseEvent(recv_guard, frigg::move(event));
			recv_guard.unlock();
			return true;
		}else{
			// post the error event
			UserEvent event(UserEvent::kTypeError, recv.submitInfo);
			event.error = kErrBufferTooSmall;

			EventHub::Guard recv_guard(&recv.eventHub->lock);
			recv.eventHub->raiseEvent(recv_guard, frigg::move(event));
			recv_guard.unlock();
			return false;
		}
	}else if(recv.type == kMsgStringToRing) {
		// transfer the request to the ring buffer
		frigg::SharedPtr<RingBuffer> ring_buffer(recv.ringBuffer);
		ring_buffer->doTransfer(frigg::move(send), frigg::move(recv));
		return true;
	}else{
		frigg::panicLogger.log() << "Illegal request type" << frigg::EndLog();
		__builtin_unreachable();
	}
}

void Channel::processDescriptorRequest(AsyncSendString &message, AsyncRecvString &request) {
	UserEvent event(UserEvent::kTypeRecvDescriptor, request.submitInfo);
	event.msgRequest = message.msgRequest;
	event.msgSequence = message.msgSequence;
	event.descriptor = frigg::move(message.descriptor);
		
	EventHub::Guard hub_guard(&request.eventHub->lock);
	request.eventHub->raiseEvent(hub_guard, frigg::move(event));
	hub_guard.unlock();
}

// --------------------------------------------------------
// AsyncSendString
// --------------------------------------------------------

// TODO: move this to another file
AsyncSendString::AsyncSendString(MsgType type, int64_t msg_request, int64_t msg_sequence)
: type(type), msgRequest(msg_request), msgSequence(msg_sequence), flags(0) { }

// --------------------------------------------------------
// AsyncRecvString
// --------------------------------------------------------

// TODO: move this to another file
AsyncRecvString::AsyncRecvString(MsgType type,
		KernelSharedPtr<EventHub> event_hub,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info)
: type(type), eventHub(frigg::move(event_hub)), submitInfo(submit_info),
		filterRequest(filter_request), filterSequence(filter_sequence), flags(0) { }

// --------------------------------------------------------
// FullPipe
// --------------------------------------------------------

void FullPipe::create(KernelSharedPtr<FullPipe> &pipe,
		KernelSharedPtr<Endpoint> &end1, KernelSharedPtr<Endpoint> &end2) {
	pipe = frigg::makeShared<FullPipe>(*kernelAlloc);
	end1 = frigg::makeShared<Endpoint>(*kernelAlloc, pipe, 0, 1);
	end2 = frigg::makeShared<Endpoint>(*kernelAlloc, pipe, 1, 0);
}

Channel &FullPipe::getChannel(size_t index) {
	return p_channels[index];
}

// --------------------------------------------------------
// Endpoint
// --------------------------------------------------------

Endpoint::Endpoint(KernelSharedPtr<FullPipe> pipe,
		size_t read_index, size_t write_index)
: p_pipe(pipe), p_readIndex(read_index), p_writeIndex(write_index) { }

Endpoint::~Endpoint() {
	for(size_t i = 0; i < 2; i++) {
		Channel &channel = p_pipe->getChannel(i);
		Channel::Guard guard(&channel.lock);
		channel.close(guard);
	}
}

KernelUnsafePtr<FullPipe> Endpoint::getPipe() {
	return p_pipe;
}

size_t Endpoint::getReadIndex() {
	return p_readIndex;
}
size_t Endpoint::getWriteIndex() {
	return p_writeIndex;
}

// --------------------------------------------------------
// Server
// --------------------------------------------------------

Server::Server() : p_acceptRequests(*kernelAlloc),
		p_connectRequests(*kernelAlloc) { }

void Server::submitAccept(Guard &guard, KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	assert(guard.protects(&lock));

	AcceptRequest request(frigg::move(event_hub), submit_info);
	
	if(!p_connectRequests.empty()) {
		processRequests(request, p_connectRequests.front());
		p_connectRequests.removeFront();
	}else{
		p_acceptRequests.addBack(frigg::move(request));
	}
}

void Server::submitConnect(Guard &guard, KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	assert(guard.protects(&lock));

	ConnectRequest request(frigg::move(event_hub), submit_info);

	if(!p_acceptRequests.empty()) {
		processRequests(p_acceptRequests.front(), request);
		p_acceptRequests.removeFront();
	}else{
		p_connectRequests.addBack(frigg::move(request));
	}
}

void Server::processRequests(const AcceptRequest &accept,
		const ConnectRequest &connect) {
	KernelSharedPtr<FullPipe> pipe;
	KernelSharedPtr<Endpoint> end1, end2;
	FullPipe::create(pipe, end1, end2);

	UserEvent accept_event(UserEvent::kTypeAccept, accept.submitInfo);
	accept_event.endpoint = frigg::move(end1);
	
	EventHub::Guard accept_hub_guard(&accept.eventHub->lock);
	accept.eventHub->raiseEvent(accept_hub_guard, frigg::move(accept_event));
	accept_hub_guard.unlock();

	UserEvent connect_event(UserEvent::kTypeConnect, connect.submitInfo);
	connect_event.endpoint = frigg::move(end2);
	
	EventHub::Guard connect_hub_guard(&connect.eventHub->lock);
	connect.eventHub->raiseEvent(connect_hub_guard, frigg::move(connect_event));
	connect_hub_guard.unlock();
}

// --------------------------------------------------------
// Server::AcceptRequest
// --------------------------------------------------------

Server::AcceptRequest::AcceptRequest(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(frigg::move(event_hub)), submitInfo(submit_info) { }

// --------------------------------------------------------
// Server::ConnectRequest
// --------------------------------------------------------

Server::ConnectRequest::ConnectRequest(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(frigg::move(event_hub)), submitInfo(submit_info) { }

} // namespace thor

