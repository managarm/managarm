
#include "kernel.hpp"

namespace traits = frigg::traits;
namespace debug = frigg::debug;

namespace thor {

// --------------------------------------------------------
// Channel
// --------------------------------------------------------

Channel::Channel() : p_messages(*kernelAlloc),
		p_requests(*kernelAlloc) { }

void Channel::sendString(const uint8_t *user_buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence) {
	uint8_t *kernel_buffer = (uint8_t *)kernelAlloc->allocate(length);
	memcpy(kernel_buffer, user_buffer, length);
	
	Message message(kMsgString, msg_request, msg_sequence);
	message.kernelBuffer = kernel_buffer;
	message.length = length;

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
		p_messages.addBack(traits::move(message));
}

void Channel::sendDescriptor(AnyDescriptor &&descriptor,
		int64_t msg_request, int64_t msg_sequence) {
	Message message(kMsgDescriptor, msg_request, msg_sequence);
	message.descriptor = traits::move(descriptor);

	for(auto it = p_requests.frontIter(); it.okay(); ++it) {
		if(!matchRequest(message, *it))
			continue;
		
		processDescriptorRequest(message, *it);
		p_requests.remove(it);
		return;
	}

	p_messages.addBack(traits::move(message));
}

void Channel::submitRecvString(KernelSharedPtr<EventHub> &&event_hub,
		uint8_t *user_buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info) {
	Request request(kMsgString, traits::move(event_hub),
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
		p_requests.addBack(traits::move(request));
}

void Channel::submitRecvDescriptor(KernelSharedPtr<EventHub> &&event_hub,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info) {
	Request request(kMsgDescriptor, traits::move(event_hub),
			filter_request, filter_sequence, submit_info);

	for(auto it = p_messages.frontIter(); it.okay(); ++it) {
		if(!matchRequest(*it, request))
			continue;
		
		processDescriptorRequest(*it, request);
		p_messages.remove(it);
		return;
	}
	
	p_requests.addBack(traits::move(request));
}

bool Channel::matchRequest(const Message &message, const Request &request) {
	if(request.type != message.type)
		return false;
	
	if(request.filterRequest != -1)
		if(request.filterRequest != message.msgRequest)
			return false;
	
	if(request.filterSequence != -1)
		if(request.filterSequence != message.msgSequence)
			return false;
	
	return true;
}

bool Channel::processStringRequest(Message &message, Request &request) {
	if(message.length > request.maxLength) {
		UserEvent event(UserEvent::kTypeRecvStringError, request.submitInfo);
		event.error = kErrBufferTooSmall;

		EventHub::Guard hub_guard(&request.eventHub->lock);
		request.eventHub->raiseEvent(hub_guard, traits::move(event));
		hub_guard.unlock();
		return false;
	}else{
		UserEvent event(UserEvent::kTypeRecvStringTransfer, request.submitInfo);
		event.msgRequest = message.msgRequest;
		event.msgSequence = message.msgSequence;
		event.kernelBuffer = message.kernelBuffer;
		event.userBuffer = request.userBuffer;
		event.length = message.length;
		
		EventHub::Guard hub_guard(&request.eventHub->lock);
		request.eventHub->raiseEvent(hub_guard, traits::move(event));
		hub_guard.unlock();
		return true;
	}
}

void Channel::processDescriptorRequest(Message &message, Request &request) {
	UserEvent event(UserEvent::kTypeRecvDescriptor, request.submitInfo);
	event.msgRequest = message.msgRequest;
	event.msgSequence = message.msgSequence;
	event.descriptor = traits::move(message.descriptor);
		
	EventHub::Guard hub_guard(&request.eventHub->lock);
	request.eventHub->raiseEvent(hub_guard, traits::move(event));
	hub_guard.unlock();
}

// --------------------------------------------------------
// Channel::Message
// --------------------------------------------------------

Channel::Message::Message(MsgType type, int64_t msg_request, int64_t msg_sequence)
	: type(type), kernelBuffer(nullptr), length(0),
		msgRequest(msg_request), msgSequence(msg_sequence) { }

// --------------------------------------------------------
// Channel::Request
// --------------------------------------------------------

Channel::Request::Request(MsgType type,
		KernelSharedPtr<EventHub> &&event_hub,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info)
	: type(type), eventHub(traits::move(event_hub)), submitInfo(submit_info),
		userBuffer(nullptr), maxLength(0),
		filterRequest(filter_request), filterSequence(filter_sequence) { }

// --------------------------------------------------------
// BiDirectionPipe
// --------------------------------------------------------

BiDirectionPipe::BiDirectionPipe() {

}

Channel *BiDirectionPipe::getFirstChannel() {
	return &p_firstChannel;
}

Channel *BiDirectionPipe::getSecondChannel() {
	return &p_secondChannel;
}

// --------------------------------------------------------
// Server
// --------------------------------------------------------

Server::Server() : p_acceptRequests(*kernelAlloc),
		p_connectRequests(*kernelAlloc) { }

void Server::submitAccept(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	AcceptRequest request(traits::move(event_hub), submit_info);
	
	if(!p_connectRequests.empty()) {
		processRequests(request, p_connectRequests.front());
		p_connectRequests.removeFront();
	}else{
		p_acceptRequests.addBack(traits::move(request));
	}
}

void Server::submitConnect(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info) {
	ConnectRequest request(traits::move(event_hub), submit_info);

	if(!p_acceptRequests.empty()) {
		processRequests(p_acceptRequests.front(), request);
		p_acceptRequests.removeFront();
	}else{
		p_connectRequests.addBack(traits::move(request));
	}
}

void Server::processRequests(const AcceptRequest &accept,
		const ConnectRequest &connect) {
	auto pipe = frigg::makeShared<BiDirectionPipe>(*kernelAlloc);
	KernelSharedPtr<BiDirectionPipe> copy(pipe);

	UserEvent accept_event(UserEvent::kTypeAccept, accept.submitInfo);
	accept_event.pipe = traits::move(pipe);
	
	EventHub::Guard accept_hub_guard(&accept.eventHub->lock);
	accept.eventHub->raiseEvent(accept_hub_guard, traits::move(accept_event));
	accept_hub_guard.unlock();

	UserEvent connect_event(UserEvent::kTypeConnect, connect.submitInfo);
	connect_event.pipe = traits::move(copy);
	
	EventHub::Guard connect_hub_guard(&connect.eventHub->lock);
	connect.eventHub->raiseEvent(connect_hub_guard, traits::move(connect_event));
	connect_hub_guard.unlock();
}

// --------------------------------------------------------
// Server::AcceptRequest
// --------------------------------------------------------

Server::AcceptRequest::AcceptRequest(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(traits::move(event_hub)), submitInfo(submit_info) { }

// --------------------------------------------------------
// Server::ConnectRequest
// --------------------------------------------------------

Server::ConnectRequest::ConnectRequest(KernelSharedPtr<EventHub> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(traits::move(event_hub)), submitInfo(submit_info) { }

} // namespace thor

