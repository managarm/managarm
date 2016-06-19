
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
	for(auto it = _recvStringQueue.frontIter(); it; ++it) {
		if(!matchStringRequest(send, *it))
			continue;
		
		if(processStringRequest(send, (*it).toShared())) {
			_recvStringQueue.remove(it);
			// don't queue the message if a request succeeds
			queue_message = false;
			break;
		}
	}

	if(queue_message)
		_sendStringQueue.addBack(frigg::move(send));
	return kErrSuccess;
}

Error Channel::sendDescriptor(Guard &guard, frigg::SharedPtr<AsyncSendDescriptor> send) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	for(auto it = _recvDescriptorQueue.frontIter(); it; ++it) {
		if(!matchDescriptorRequest(send, *it))
			continue;
		
		processDescriptorRequest(send, (*it).toShared());
		_recvDescriptorQueue.remove(it);
		return kErrSuccess;
	}

	_sendDescriptorQueue.addBack(frigg::move(send));
	return kErrSuccess;
}

Error Channel::submitRecvString(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	bool queue_request = true;
	for(auto it = _sendStringQueue.frontIter(); it; ++it) {
		if(!matchStringRequest(*it, recv))
			continue;
		
		if(processStringRequest((*it).toShared(), recv))
			_sendStringQueue.remove(it);
		// NOTE: we never queue failed requests
		queue_request = false;
		break;
	}
	
	if(queue_request)
		_recvStringQueue.addBack(frigg::move(recv));
	return kErrSuccess;
}

Error Channel::submitRecvDescriptor(Guard &guard, frigg::SharedPtr<AsyncRecvDescriptor> recv) {
	assert(guard.protects(&lock));

	if(_wasClosed)
		return kErrPipeClosed;

	for(auto it = _sendDescriptorQueue.frontIter(); it; ++it) {
		if(!matchDescriptorRequest(*it, recv))
			continue;
		
		processDescriptorRequest((*it).toShared(), recv);
		_sendDescriptorQueue.remove(it);
		return kErrSuccess;
	}
	
	_recvDescriptorQueue.addBack(frigg::move(recv));
	return kErrSuccess;
}

void Channel::close(Guard &guard) {
	// TODO: just cancel all requests
	assert(_sendStringQueue.empty());
	assert(_sendDescriptorQueue.empty());
	assert(_recvStringQueue.empty());
	assert(_recvDescriptorQueue.empty());

/*	while(!_sendQueue.empty())
		_sendQueue.removeFront();

	while(!_recvQueue.empty()) {
		frigg::SharedPtr<AsyncRecvString> recv = _recvQueue.removeFront();

		UserEvent event(UserEvent::kTypeError, recv->submitInfo);
		event.error = kErrPipeClosed;

		frigg::SharedPtr<EventHub> event_hub = recv->eventHub.grab();
		assert(event_hub);
		EventHub::Guard hub_guard(&event_hub->lock);
		event_hub->raiseEvent(hub_guard, frigg::move(event));
		hub_guard.unlock();
	}*/

	_wasClosed = true;
}

bool Channel::matchStringRequest(frigg::UnsafePtr<AsyncSendString> send,
		frigg::UnsafePtr<AsyncRecvString> recv) {
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

bool Channel::matchDescriptorRequest(frigg::UnsafePtr<AsyncSendDescriptor> send,
		frigg::UnsafePtr<AsyncRecvDescriptor> recv) {
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
			
			recv->error = kErrSuccess;
			recv->msgRequest = send->msgRequest;
			recv->msgSequence = send->msgSequence;
			recv->length = send->kernelBuffer.size();

			AsyncOperation::complete(frigg::move(send));
			AsyncOperation::complete(frigg::move(recv));
			return true;
		}else{
			// post the error event
			assert(!"Fix recv error");
			{
/*				UserEvent event(UserEvent::kTypeError, recv->submitInfo);
				event.error = kErrBufferTooSmall;

				frigg::SharedPtr<EventHub> event_hub = recv->eventHub.grab();
				assert(event_hub);
				EventHub::Guard hub_guard(&event_hub->lock);
				event_hub->raiseEvent(hub_guard, frigg::move(event));*/
			}
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

void Channel::processDescriptorRequest(frigg::SharedPtr<AsyncSendDescriptor> send,
		frigg::SharedPtr<AsyncRecvDescriptor> recv) {
	frigg::SharedPtr<Universe> universe = recv->universe.grab();
	assert(universe);

	Handle handle;
	{
		Universe::Guard universe_guard(&universe->lock);
		handle = universe->attachDescriptor(universe_guard,
				frigg::move(send->descriptor));
	}

	recv->error = kErrSuccess;
	recv->msgRequest = send->msgRequest;
	recv->msgSequence = send->msgSequence;
	recv->handle = handle;

	AsyncOperation::complete(frigg::move(send));
	AsyncOperation::complete(frigg::move(recv));
}

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

void Server::submitAccept(Guard &guard, frigg::SharedPtr<AsyncAccept> request) {
	assert(guard.protects(&lock));

	if(!_connectQueue.empty()) {
		processRequests(frigg::move(request), _connectQueue.front().toShared());
		_connectQueue.removeFront();
	}else{
		_acceptQueue.addBack(frigg::move(request));
	}
}

void Server::submitConnect(Guard &guard, frigg::SharedPtr<AsyncConnect> request) {
	assert(guard.protects(&lock));

	if(!_acceptQueue.empty()) {
		processRequests(_acceptQueue.front().toShared(), frigg::move(request));
		_acceptQueue.removeFront();
	}else{
		_connectQueue.addBack(frigg::move(request));
	}
}

void Server::processRequests(frigg::SharedPtr<AsyncAccept> accept,
		frigg::SharedPtr<AsyncConnect> connect) {
	KernelSharedPtr<FullPipe> pipe;
	KernelSharedPtr<Endpoint> end1, end2;
	FullPipe::create(pipe, end1, end2);

	frigg::SharedPtr<Universe> accept_universe = accept->universe.grab();
	assert(accept_universe);
	{
		Universe::Guard universe_guard(&accept_universe->lock);
		accept->handle = accept_universe->attachDescriptor(universe_guard,
				EndpointDescriptor(frigg::move(end1)));
	}
	
	frigg::SharedPtr<Universe> connect_universe = connect->universe.grab();
	assert(connect_universe);
	{
		Universe::Guard universe_guard(&connect_universe->lock);
		connect->handle = connect_universe->attachDescriptor(universe_guard,
				EndpointDescriptor(frigg::move(end2)));
	}

	AsyncOperation::complete(frigg::move(accept));
	AsyncOperation::complete(frigg::move(connect));
}

} // namespace thor

