
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

// --------------------------------------------------------
// Channel
// --------------------------------------------------------

Channel::Channel() : p_messages(*kernelAlloc),
		p_requests(*kernelAlloc) { }

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

void Channel::submitRecvString(SharedPtr<EventHub, KernelAlloc> &&event_hub,
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

// --------------------------------------------------------
// Channel::Message
// --------------------------------------------------------

Channel::Message::Message(uint8_t *buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence)
	: kernelBuffer(buffer), length(length),
		msgRequest(msg_request), msgSequence(msg_sequence) { }

// --------------------------------------------------------
// Channel::Request
// --------------------------------------------------------

Channel::Request::Request(SharedPtr<EventHub, KernelAlloc> &&event_hub,
		int64_t filter_request, int64_t filter_sequence,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info),
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

void Server::submitAccept(SharedPtr<EventHub, KernelAlloc> &&event_hub,
		SubmitInfo submit_info) {
	AcceptRequest request(util::move(event_hub), submit_info);
	
	if(!p_connectRequests.empty()) {
		processRequests(request, p_connectRequests.front());
		p_connectRequests.removeFront();
	}else{
		p_acceptRequests.addBack(util::move(request));
	}
}

void Server::submitConnect(SharedPtr<EventHub, KernelAlloc> &&event_hub,
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
	auto pipe = makeShared<BiDirectionPipe>(*kernelAlloc);
	SharedPtr<BiDirectionPipe, KernelAlloc> copy(pipe);

	accept.eventHub->raiseAcceptEvent(util::move(pipe),
			accept.submitInfo);
	connect.eventHub->raiseConnectEvent(util::move(copy),
			connect.submitInfo);
}

// --------------------------------------------------------
// Server::AcceptRequest
// --------------------------------------------------------

Server::AcceptRequest::AcceptRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info) { }

// --------------------------------------------------------
// Server::ConnectRequest
// --------------------------------------------------------

Server::ConnectRequest::ConnectRequest(SharedPtr<EventHub, KernelAlloc> &&event_hub,
		SubmitInfo submit_info)
	: eventHub(util::move(event_hub)), submitInfo(submit_info) { }

} // namespace thor

