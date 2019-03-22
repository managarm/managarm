
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

static void transfer(OfferAccept, StreamNode *offer, StreamNode *accept) {
	offer->_error = kErrSuccess;
	offer->complete();

	// TODO: move universe and lane?
	accept->_error = kErrSuccess;
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
		assert(!stream->_laneBroken[lane]);

		stream->_laneBroken[lane] = true;

		while(!stream->_processQueue[!lane].empty()) {
			auto item = stream->_processQueue[!lane].pop_front();
			_cancelItem(item, kErrEndOfLane);
		}
	}
	return true;
}

Stream::Stream()
: _laneBroken{false, false}, _laneShutDown{false, false} {
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
	_laneShutDown[lane] = true;

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

	// Note: Try to do as little work as possible while holding the lock.
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);
		assert(!_laneBroken[p]);

		if(_processQueue[q].empty()) {
			// Accept and Offer create new streams.
			if(OfferBase::classOf(*u) || AcceptBase::classOf(*u)) {
				// Initially there will be 3 references to the stream:
				// * One reference for the original shared pointer.
				// * One reference for each of the two lanes.
				auto conversation = frigg::makeShared<Stream>(*kernelAlloc);
				conversation.control().counter()->setRelaxed(3);
				u->_lane = LaneHandle{adoptLane, conversation, p};
				u->_pairedLane = LaneHandle{adoptLane, conversation, q};
			}

			if(_laneShutDown[p]) {
				_cancelItem(u, kErrLaneShutdown);
			}else if(_laneBroken[q] || _laneShutDown[q]) {
				_cancelItem(u, kErrEndOfLane);
			}else{
				_processQueue[p].push_back(u);
			}

			return u->_lane;
		}

		// If any lane is broken or shut down, the _processQueue should have been empty.
		assert(!_laneShutDown[p]);
		assert(!_laneBroken[q] && !_laneShutDown[q]);

		// Both lanes have items; we need to process them.
		v = _processQueue[q].pop_front();
	}

	// Do the main work here, after we released the lock.
	LaneHandle conversation;
	if(OfferBase::classOf(*u) && AcceptBase::classOf(*v)) {
		// "Steal" the paired lane from v.
		conversation = std::move(v->_pairedLane);
		assert(conversation);
		u->_lane = conversation;
		transfer(OfferAccept{}, u, v);
	}else if(OfferBase::classOf(*v) && AcceptBase::classOf(*u)) {
		// "Steal" the paired lane from v.
		conversation = std::move(v->_pairedLane);
		assert(conversation);
		u->_lane = conversation;
		transfer(OfferAccept{}, v, u);
	}else if(ImbueCredentialsBase::classOf(*u)
			&& ExtractCredentialsBase::classOf(*v)) {
		transfer(ImbueExtract{}, u, v);
	}else if(ImbueCredentialsBase::classOf(*v)
			&& ExtractCredentialsBase::classOf(*u)) {
		transfer(ImbueExtract{}, v, u);
	}else if(SendFromBufferBase::classOf(*u)
			&& RecvInlineBase::classOf(*v)) {
		transfer(SendRecvInline{}, u, v);
	}else if(SendFromBufferBase::classOf(*v)
			&& RecvInlineBase::classOf(*u)) {
		transfer(SendRecvInline{}, v, u);
	}else if(SendFromBufferBase::classOf(*u)
			&& RecvToBufferBase::classOf(*v)) {
		transfer(SendRecvBuffer{}, u, v);
	}else if(SendFromBufferBase::classOf(*v)
			&& RecvToBufferBase::classOf(*u)) {
		transfer(SendRecvBuffer{}, v, u);
	}else if(PushDescriptorBase::classOf(*u)
			&& PullDescriptorBase::classOf(*v)) {
		transfer(PushPull{}, u, v);
	}else if(PushDescriptorBase::classOf(*v)
			&& PullDescriptorBase::classOf(*u)) {
		transfer(PushPull{}, v, u);
	}else{
		frigg::infoLogger() << u->tag()
				<< " vs. " << v->tag() << frigg::endLog;
		assert(!"Operations do not match");
		__builtin_trap();
	}

	return std::move(conversation);
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

