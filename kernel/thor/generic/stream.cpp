
#include <thor-internal/stream.hpp>

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
	offer->_error = Error::success;
	offer->complete();

	// TODO: move universe and lane?
	accept->_error = Error::success;
	accept->complete();
}

static void transfer(ImbueExtract, StreamNode *from, StreamNode *to) {
	auto credentials = from->_inCredentials;

	from->_error = Error::success;
	from->complete();

	to->_error = Error::success;
	to->_transmitCredentials = credentials;
	to->complete();
}

static void transfer(SendRecvInline, StreamNode *from, StreamNode *to) {
	auto buffer = std::move(from->_inBuffer);

	if(buffer.size() <= to->_maxLength) {
		from->_error = Error::success;
		from->complete();

		to->_error = Error::success;
		to->_transmitBuffer = std::move(buffer);
		to->complete();
	}else{
		from->_error = Error::bufferTooSmall;
		from->complete();

		to->_error = Error::bufferTooSmall;
		to->complete();
	}
}

static void transfer(SendRecvBuffer, StreamNode *from, StreamNode *to) {
	auto buffer = std::move(from->_inBuffer);

	if(buffer.size() <= to->_inAccessor.length()) {
		auto error = to->_inAccessor.write(0, buffer.data(), buffer.size());
		if(error != Error::success) {
			from->_error = Error::success;
			from->complete();

			to->_error = error;
			to->_actualLength = 0;
			to->complete();
		}else{
			from->_error = Error::success;
			from->complete();

			to->_error = Error::success;
			to->_actualLength = buffer.size();
			to->complete();
		}
	}else{
		from->_error = Error::bufferTooSmall;
		from->complete();

		to->_error = Error::bufferTooSmall;
		to->complete();
	}
}

static void transfer(PushPull, StreamNode *push, StreamNode *pull) {
	auto descriptor = std::move(push->_inDescriptor);

	push->_error = Error::success;
	push->complete();

	pull->_error = Error::success;
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
		auto s = u->_transmitLane.getStream();
		bool laneShutdown = false;
		bool laneBroken = false;
		{
			// p/q is the number of the local/remote lane.
			// u/v is the local/remote item that we are processing.
			int p = u->_transmitLane.getLane();
			assert(!(p & ~int(1)));
			int q = 1 - p;

			auto irq_lock = frg::guard(&irqMutex());
			auto lock = frg::guard(&s->_mutex);
			assert(!s->_laneBroken[p]);

			if(s->_laneShutDown[p]) {
				assert(s->_processQueue[q].empty());
				laneShutdown = true;
			}else if(s->_laneBroken[q] || s->_laneShutDown[q]) {
				assert(s->_processQueue[q].empty());
				laneBroken = true;
			}else{
				// If both lanes have items, we need to process them.
				// Otherwise, we just queue the new node.
				if(s->_processQueue[q].empty()) {
					s->_processQueue[p].push_back(u);
					continue;
				}
				v = s->_processQueue[q].pop_front();
			}
		}

		if(laneShutdown) {
			s->_cancelItem(u, Error::laneShutdown);
			continue;
		}
		if(laneBroken) {
			s->_cancelItem(u, Error::endOfLane);
			continue;
		}

		// Make sure that we only need to consider one permutation of tags.
		if(getStreamOrientation(u->tag()) < getStreamOrientation(v->tag()))
			std::swap(u, v);

		// Do the main work here, after we released the lock.
		if(u->tag() == kTagOffer && v->tag() == kTagAccept) {
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
		}else if(u->tag() == kTagImbueCredentials && v->tag() == kTagExtractCredentials) {
			transfer(ImbueExtract{}, u, v);
		}else if(u->tag() == kTagSendFromBuffer && v->tag() == kTagRecvInline) {
			transfer(SendRecvInline{}, u, v);
		}else if(u->tag() == kTagSendFromBuffer && v->tag() == kTagRecvToBuffer) {
			transfer(SendRecvBuffer{}, u, v);
		}else if(u->tag() == kTagPushDescriptor && v->tag() == kTagPullDescriptor) {
			transfer(PushPull{}, u, v);
		}else{
			u->_error = Error::transmissionMismatch;
			u->complete();

			v->_error = Error::transmissionMismatch;
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

	frg::intrusive_list<
		StreamNode,
		frg::locate_member<
			StreamNode,
			frg::default_list_hook<StreamNode>,
			&StreamNode::processQueueItem
		>
	> pending;

	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&stream->_mutex);
		assert(!stream->_laneBroken[lane]);

		stream->_laneBroken[lane] = true;
		pending.splice(pending.end(), stream->_processQueue[!lane]);
	}

	while(!pending.empty()) {
		auto item = pending.pop_front();
		_cancelItem(item, Error::endOfLane);
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
//	infoLogger() << "\e[31mClosing stream\e[0m" << frg::endlog;
}

void Stream::shutdownLane(int lane) {
	frg::intrusive_list<
		StreamNode,
		frg::locate_member<
			StreamNode,
			frg::default_list_hook<StreamNode>,
			&StreamNode::processQueueItem
		>
	> pendingOnThisLane, pendingOnOtherLane;

	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);
		assert(!_laneBroken[lane]);

		_laneShutDown[lane] = true;
		pendingOnThisLane.splice(pendingOnThisLane.end(), _processQueue[lane]);
		pendingOnOtherLane.splice(pendingOnOtherLane.end(), _processQueue[!lane]);
	}

	while(!pendingOnThisLane.empty()) {
		auto item = pendingOnThisLane.pop_front();
		_cancelItem(item, Error::laneShutdown);
	}

	while(!pendingOnOtherLane.empty()) {
		auto item = pendingOnOtherLane.pop_front();
		_cancelItem(item, Error::endOfLane);
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

