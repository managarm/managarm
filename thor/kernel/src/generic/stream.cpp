
#include "kernel.hpp"

namespace thor {

LaneHandle::LaneHandle(const LaneHandle &other)
: _stream(other._stream), _lane(other._lane) {
	if(_stream)
		Stream::incrementPeers(_stream.get(), _lane);
}

LaneHandle::~LaneHandle() {
	if(!_stream)
		return;

	if(Stream::decrementPeers(_stream.get(), _lane))
		_stream.control().decrement();
}

struct OfferAccept { };
struct ImbueExtract { };
struct SendRecvInline { };
struct SendRecvBuffer { };
struct PushPull { };

static void transfer(OfferAccept, StreamNode *offer, StreamNode *accept, LaneHandle lane) {
	offer->_error = kErrSuccess;
	offer->complete();

	// TODO: move universe and lane?
	accept->_error = kErrSuccess;
	accept->_lane = std::move(lane);
	accept->complete();
}

static void transfer(ImbueExtract, StreamNode *from, StreamNode *to) {
	auto credentials = from->_inCredentials;

	from->_error = kErrSuccess;
	from->complete();

	to->_error = kErrSuccess;
	to->_transmitCredentials = credentials;
	to->complete();
}

static void transfer(SendRecvInline, StreamNode *from, StreamNode *to) {
	auto buffer = std::move(from->_inBuffer);

	from->_error = kErrSuccess;
	from->complete();

	to->_error = kErrSuccess;
	to->_transmitBuffer = std::move(buffer);
	to->complete();
}

static void transfer(SendRecvBuffer, StreamNode *from, StreamNode *to) {
	auto buffer = std::move(from->_inBuffer);

	if(buffer.size() <= to->_inAccessor.length()) {
		auto error = to->_inAccessor.write(0, buffer.data(), buffer.size());
		if(error) {
			from->_error = kErrSuccess;
			from->complete();

			to->_error = error;
			to->_actualLength = 0;
			to->complete();
		}else{
			from->_error = kErrSuccess;
			from->complete();

			to->_error = kErrSuccess;
			to->_actualLength = buffer.size();
			to->complete();
		}
	}else{
		from->_error = kErrBufferTooSmall;
		from->complete();

		to->_error = kErrBufferTooSmall;
		to->complete();
	}
}

static void transfer(PushPull, StreamNode *push, StreamNode *pull) {
	auto descriptor = std::move(push->_inDescriptor);

	push->_error = kErrSuccess;
	push->complete();

	pull->_error = kErrSuccess;
	pull->_descriptor = std::move(descriptor);
	pull->complete();
}

void Stream::incrementPeers(Stream *stream, int lane) {
	auto count = stream->_peerCount[lane].fetch_add(1, std::memory_order_relaxed);
	assert(count);
}

bool Stream::decrementPeers(Stream *stream, int lane) {
	auto count = stream->_peerCount[lane].fetch_sub(1, std::memory_order_release);
	if(count > 1)
		return false;
	
	std::atomic_thread_fence(std::memory_order_acquire);
	
// TODO: remove debugging messages?
//	frigg::infoLogger() << "\e[31mClosing lane " << lane << "\e[0m" << frigg::endLog;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&stream->_mutex);

		if(!stream->_laneBroken[lane]) {
			stream->_laneBroken[lane] = true;

			while(!stream->_processQueue[!lane].empty()) {
				auto item = stream->_processQueue[!lane].pop_front(); 
				_cancelItem(item, kErrEndOfLane);
			}
		}
	}
	return true;
}

Stream::Stream()
: _conversationQueue(*kernelAlloc), _laneBroken{false, false} {
	_peerCount[0].store(1, std::memory_order_relaxed);
	_peerCount[1].store(1, std::memory_order_relaxed);
}

Stream::~Stream() {
// TODO: remove debugging messages?
//	frigg::infoLogger() << "\e[31mClosing stream\e[0m" << frigg::endLog;
}

void Stream::shutdownLane(int lane) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	assert(!_laneBroken[lane]);

//	frigg::infoLogger() << "Shutting down lane" << frigg::endLog;
	_laneBroken[lane] = true;
	
	while(!_conversationQueue.empty())
		_conversationQueue.removeFront(); 

	while(!_processQueue[lane].empty()) {
		auto item = _processQueue[lane].pop_front(); 
		_cancelItem(item, kErrLaneShutdown);
	}

	while(!_processQueue[!lane].empty()) {
		auto item = _processQueue[!lane].pop_front(); 
		_cancelItem(item, kErrEndOfLane);
	}
}

void Stream::_cancelItem(StreamNode *item, Error error) {
	item->_error = error;
	item->complete();
}

LaneHandle Stream::transmit(int p, StreamNode *u) {
	// p/q is the number of the local/remote lane.
	// u/v is the local/remote item that we are processing.
	assert(!(p & ~int(1)));
	int q = 1 - p;
	StreamNode *v = nullptr;

	// The stream created by Accept/Offer.
	frigg::SharedPtr<Stream> conversation;

	// Note: Try to do as little work as possible while holding the lock.
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);
		assert(!_laneBroken[p]);

		// Accept and Offer create new streams.
		if(OfferBase::classOf(*u) || AcceptBase::classOf(*u)) {
			if(_processQueue[q].empty()) {
				// Initially there will be 3 references to the stream:
				// * One reference for the original shared pointer.
				// * One reference for each of the two lanes.
				conversation = frigg::makeShared<Stream>(*kernelAlloc);
				conversation.control().counter()->setRelaxed(3);
				
				// We will adopt exactly two LaneHandle objects per lane.
				conversation->_peerCount[0].store(2, std::memory_order_relaxed);
				conversation->_peerCount[1].store(2, std::memory_order_relaxed);

				if(_laneBroken[q]) {
					// This will immediately break the new lane.
					LaneHandle{adoptLane, conversation, q};
				}else{
					_conversationQueue.addBack(conversation);
				}
			}else{
				conversation = _conversationQueue.removeFront();
			}
		}

		if(_processQueue[q].empty()) {
			if(_laneBroken[q]) {
				_cancelItem(u, kErrEndOfLane);
			}else{
				_processQueue[p].push_back(u);
			}
		}else{
			// Both queues are non-empty and we process both items.
			v = _processQueue[q].pop_front();
		}
	}

	// Do the main work here, after we released the lock.
	if(v) {
		if(OfferBase::classOf(*u)
				&& AcceptBase::classOf(*v)) {
			LaneHandle lane1(adoptLane, conversation, p);
			LaneHandle lane2(adoptLane, conversation, q);

			transfer(OfferAccept{}, u, v, std::move(lane2));

			return LaneHandle(adoptLane, conversation, p);
		}else if(OfferBase::classOf(*v)
				&& AcceptBase::classOf(*u)) {
			LaneHandle lane1(adoptLane, conversation, p);
			LaneHandle lane2(adoptLane, conversation, q);

			transfer(OfferAccept{}, v, u, std::move(lane1));
			
			return LaneHandle(adoptLane, conversation, p);
		}else if(ImbueCredentialsBase::classOf(*u)
				&& ExtractCredentialsBase::classOf(*v)) {
			transfer(ImbueExtract{}, u, v);
			return LaneHandle();
		}else if(ImbueCredentialsBase::classOf(*v)
				&& ExtractCredentialsBase::classOf(*u)) {
			transfer(ImbueExtract{}, v, u);
			return LaneHandle();
		}else if(SendFromBufferBase::classOf(*u)
				&& RecvInlineBase::classOf(*v)) {
			transfer(SendRecvInline{}, u, v);
			return LaneHandle();
		}else if(SendFromBufferBase::classOf(*v)
				&& RecvInlineBase::classOf(*u)) {
			transfer(SendRecvInline{}, v, u);
			return LaneHandle();
		}else if(SendFromBufferBase::classOf(*u)
				&& RecvToBufferBase::classOf(*v)) {
			transfer(SendRecvBuffer{}, u, v);
			return LaneHandle();
		}else if(SendFromBufferBase::classOf(*v)
				&& RecvToBufferBase::classOf(*u)) {
			transfer(SendRecvBuffer{}, v, u);
			return LaneHandle();
		}else if(PushDescriptorBase::classOf(*u)
				&& PullDescriptorBase::classOf(*v)) {
			transfer(PushPull{}, u, v);
			return LaneHandle();
		}else if(PushDescriptorBase::classOf(*v)
				&& PullDescriptorBase::classOf(*u)) {
			transfer(PushPull{}, v, u);
			return LaneHandle();
		}else{
			frigg::infoLogger() << u->tag()
					<< " vs. " << v->tag() << frigg::endLog;
			assert(!"Operations do not match");
			__builtin_trap();
		}
	}else{
		if(OfferBase::classOf(*u) || AcceptBase::classOf(*u)) {
			return LaneHandle{adoptLane, conversation, p};
		}else{
			return LaneHandle{};
		}
	}
}

frigg::Tuple<LaneHandle, LaneHandle> createStream() {
	auto stream = frigg::makeShared<Stream>(*kernelAlloc);
	stream.control().counter()->setRelaxed(2);
	LaneHandle handle1(adoptLane, stream, 0);
	LaneHandle handle2(adoptLane, stream, 1);
	stream.release();
	return frigg::makeTuple(handle1, handle2);
}

} // namespace thor

