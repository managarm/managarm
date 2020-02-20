
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

	if(buffer.size() <= to->_maxLength) {
		from->_error = kErrSuccess;
		from->complete();

		to->_error = kErrSuccess;
		to->_transmitBuffer = std::move(buffer);
		to->complete();
	}else{
		from->_error = kErrBufferTooSmall;
		from->complete();

		to->_error = kErrBufferTooSmall;
		to->complete();
	}
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

void Stream::Submitter::enqueue(const LaneHandle &lane, StreamList &chain) {
	while(!chain.empty()) {
		auto node = chain.pop_front();
		node->_transmitLane = lane;
		_pending.push_back(node);
	}
}

void Stream::Submitter::run() {
	while(!_pending.empty()) {
		StreamNode *u = _pending.pop_front();
		StreamNode *v = nullptr;

		// Note: Try to do as little work as possible while holding the lock.
		{
			// p/q is the number of the local/remote lane.
			// u/v is the local/remote item that we are processing.
			auto s = u->_transmitLane.getStream();
			int p = u->_transmitLane.getLane();
			assert(!(p & ~int(1)));
			int q = 1 - p;

			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&s->_mutex);
			assert(!s->_laneBroken[p]);

			if(s->_laneShutDown[p]) {
				assert(s->_processQueue[q].empty());
				s->_cancelItem(u, kErrLaneShutdown);
				continue;
			}else if(s->_laneBroken[q] || s->_laneShutDown[q]) {
				assert(s->_processQueue[q].empty());
				s->_cancelItem(u, kErrEndOfLane);
				continue;
			}

			// If both lanes have items, we need to process them.
			// Otherwise, we just queue the new node.
			if(s->_processQueue[q].empty()) {
				s->_processQueue[p].push_back(u);
				continue;
			}
			v = s->_processQueue[q].pop_front();
		}

		// Make sure that we only need to consider one permutation of tags.
		if(getStreamOrientation(u->tag()) < getStreamOrientation(v->tag()))
			std::swap(u, v);

		// Do the main work here, after we released the lock.
		if(OfferBase::classOf(*u) && AcceptBase::classOf(*v)) {
			// Initially there will be 3 references to the new stream:
			// * One reference for the original shared pointer.
			// * One reference for each of the two lanes.
			auto branch = frigg::makeShared<Stream>(*kernelAlloc);
			branch.control().counter()->setRelaxed(3);
			u->_lane = LaneHandle{adoptLane, branch, 0};
			v->_lane = LaneHandle{adoptLane, branch, 1};

			enqueue(u->_lane, u->ancillaryChain);
			enqueue(v->_lane, v->ancillaryChain);

			transfer(OfferAccept{}, u, v);
		}else if(ImbueCredentialsBase::classOf(*u)
				&& ExtractCredentialsBase::classOf(*v)) {
			transfer(ImbueExtract{}, u, v);
		}else if(SendFromBufferBase::classOf(*u)
				&& RecvInlineBase::classOf(*v)) {
			transfer(SendRecvInline{}, u, v);
		}else if(SendFromBufferBase::classOf(*u)
				&& RecvToBufferBase::classOf(*v)) {
			transfer(SendRecvBuffer{}, u, v);
		}else if(PushDescriptorBase::classOf(*u)
				&& PullDescriptorBase::classOf(*v)) {
			transfer(PushPull{}, u, v);
		}else{
			u->_error = kErrTransmissionMismatch;
			u->complete();

			v->_error = kErrTransmissionMismatch;
			v->complete();
		}
	}
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
	StreamList pending;
	pending.splice(pending.end(), item->ancillaryChain);

	item->_error = error;
	item->complete();

	while(!pending.empty()) {
		item = pending.pop_front();
		item->_error = error;
		item->complete();
	}
}

frg::tuple<LaneHandle, LaneHandle> createStream() {
	auto stream = frigg::makeShared<Stream>(*kernelAlloc);
	stream.control().counter()->setRelaxed(2);
	LaneHandle handle1(adoptLane, stream, 0);
	LaneHandle handle2(adoptLane, stream, 1);
	stream.release();
	return frg::make_tuple(handle1, handle2);
}

} // namespace thor

