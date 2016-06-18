
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Channel
// --------------------------------------------------------

Channel::Channel()
: _wasClosed(false) { }

Error Channel::sendString(Guard &guard, frigg::SharedPtr<AsyncSendString> send) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	bool queue_message = true;
	for(auto it = _recvQueue.frontIter(); it; ++it) {
		if(!matchRequest(send, *it))
			continue;
		
		if(processStringRequest(send, (*it).toShared())) {
			_recvQueue.remove(it);
			// don't queue the message if a request succeeds
			queue_message = false;
			break;
		}
	}

	if(queue_message)
		_sendQueue.addBack(frigg::move(send));
	return kErrSuccess;
}

Error Channel::sendDescriptor(Guard &guard, frigg::SharedPtr<AsyncSendString> send) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	for(auto it = _recvQueue.frontIter(); it; ++it) {
		if(!matchRequest(send, *it))
			continue;
		
		processDescriptorRequest(send, (*it).toShared());
		_recvQueue.remove(it);
		return kErrSuccess;
	}

	_sendQueue.addBack(frigg::move(send));
	return kErrSuccess;
}

Error Channel::submitRecvString(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	bool queue_request = true;
	for(auto it = _sendQueue.frontIter(); it; ++it) {
		if(!matchRequest(*it, recv))
			continue;
		
		if(processStringRequest((*it).toShared(), recv))
			_sendQueue.remove(it);
		// NOTE: we never queue failed requests
		queue_request = false;
		break;
	}
	
	if(queue_request)
		_recvQueue.addBack(frigg::move(recv));
	return kErrSuccess;
}

Error Channel::submitRecvStringToRing(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	bool queue_request = true;
	for(auto it = _sendQueue.frontIter(); it; ++it) {
		if(!matchRequest(*it, recv))
			continue;
		
		if(processStringRequest((*it).toShared(), recv))
			_sendQueue.remove(it);
		// NOTE: we never queue failed requests
		queue_request = false;
		break;
	}
	
	if(queue_request)
		_recvQueue.addBack(frigg::move(recv));
	return kErrSuccess;
}

Error Channel::submitRecvDescriptor(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	for(auto it = _sendQueue.frontIter(); it; ++it) {
		if(!matchRequest(*it, recv))
			continue;
		
		processDescriptorRequest((*it).toShared(), recv);
		_sendQueue.remove(it);
		return kErrSuccess;
	}
	
	_recvQueue.addBack(frigg::move(recv));
	return kErrSuccess;
}

void Channel::close(Guard &guard) {
	while(!_sendQueue.empty())
		_sendQueue.removeFront();

	while(!_recvQueue.empty()) {
		frigg::SharedPtr<AsyncRecvString> recv = _recvQueue.removeFront();

		UserEvent event(UserEvent::kTypeError, recv->submitInfo);
		event.error = kErrPipeClosed;

		EventHub::Guard hub_guard(&recv->eventHub->lock);
		recv->eventHub->raiseEvent(hub_guard, frigg::move(event));
		hub_guard.unlock();
	}

	_wasClosed = true;
}

bool Channel::matchRequest(frigg::UnsafePtr<AsyncSendString> send,
		frigg::UnsafePtr<AsyncRecvString> recv) {
	if(send->type == kMsgString) {
		if(recv->type != kMsgStringToBuffer && recv->type != kMsgStringToRing)
			return false;
	}else if(send->type != recv->type) {
		return false;
	}

	if((bool)(recv->flags & kFlagRequest) != (bool)(send->flags & kFlagRequest))
		return false;
	if((bool)(recv->flags & kFlagResponse) != (bool)(send->flags & kFlagResponse))
		return false;
	
	if(recv->filterRequest != -1)
		if(recv->filterRequest != send->msgRequest)
			return false;
	
	if(recv->filterSequence != -1)
		if(recv->filterSequence != send->msgSequence)
			return false;
	
	return true;
}

bool Channel::processStringRequest(frigg::SharedPtr<AsyncSendString> send,
		frigg::SharedPtr<AsyncRecvString> recv) {
	if(recv->type == kMsgStringToBuffer) {
		if(send->kernelBuffer.size() <= recv->spaceLock.length()) {
			// perform the actual data transfer
			recv->spaceLock.copyTo(send->kernelBuffer.data(), send->kernelBuffer.size());
			
			{ // post the send event
				UserEvent event(UserEvent::kTypeSendString, send->submitInfo);
			
				frigg::SharedPtr<EventHub> event_hub = send->eventHub.grab();
				assert(event_hub);
				EventHub::Guard hub_guard(&event_hub->lock);
				event_hub->raiseEvent(hub_guard, frigg::move(event));
			}

			{ // post the receive event
				UserEvent event(UserEvent::kTypeRecvStringTransferToBuffer, recv->submitInfo);
				event.length = send->kernelBuffer.size();
				event.msgRequest = send->msgRequest;
				event.msgSequence = send->msgSequence;
			
				EventHub::Guard hub_guard(&recv->eventHub->lock);
				recv->eventHub->raiseEvent(hub_guard, frigg::move(event));
			}
			return true;
		}else{
			// post the error event
			UserEvent event(UserEvent::kTypeError, recv->submitInfo);
			event.error = kErrBufferTooSmall;

			EventHub::Guard hub_guard(&recv->eventHub->lock);
			recv->eventHub->raiseEvent(hub_guard, frigg::move(event));
			hub_guard.unlock();
			return false;
		}
	}else if(recv->type == kMsgStringToRing) {
		// transfer the request to the ring buffer
		frigg::SharedPtr<RingBuffer> ring_buffer(recv->ringBuffer);
		ring_buffer->doTransfer(frigg::move(send), frigg::move(recv));
		return true;
	}else{
		frigg::panicLogger.log() << "Illegal request type" << frigg::EndLog();
		__builtin_unreachable();
	}
}

void Channel::processDescriptorRequest(frigg::SharedPtr<AsyncSendString> send,
		frigg::SharedPtr<AsyncRecvString> recv) {
	{ // post the send event
		UserEvent event(UserEvent::kTypeSendDescriptor, send->submitInfo);

		frigg::SharedPtr<EventHub> event_hub = send->eventHub.grab();
		assert(event_hub);
		EventHub::Guard hub_guard(&event_hub->lock);
		event_hub->raiseEvent(hub_guard, frigg::move(event));
	}
	
	{ // post the receive event
		UserEvent event(UserEvent::kTypeRecvDescriptor, recv->submitInfo);
		event.msgRequest = send->msgRequest;
		event.msgSequence = send->msgSequence;
		event.descriptor = frigg::move(send->descriptor);

		EventHub::Guard hub_guard(&recv->eventHub->lock);
		recv->eventHub->raiseEvent(hub_guard, frigg::move(event));
	}
}

// --------------------------------------------------------
// AsyncSendString
// --------------------------------------------------------

// TODO: move this to another file
AsyncSendString::AsyncSendString(AsyncData data, MsgType type,
		int64_t msg_request, int64_t msg_sequence)
: AsyncOperation(frigg::move(data)), type(type),
		msgRequest(msg_request), msgSequence(msg_sequence), flags(0) { }

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

