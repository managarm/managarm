
#include <thor-internal/stream.hpp>

namespace thor {

void LanePolicy::increment() const {
	stream_->peerCounter(lane_).increment();
}

void LanePolicy::decrement() const {
	if(stream_->peerCounter(lane_).decrement_and_check_if_zero()) {
		Stream::onPeersZero(stream_, lane_);
		stream_->selfPtr.policy().decrement();
	}
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

static void transfer(PushPull, StreamNode *push, StreamNode *pull) {
	auto descriptor = std::move(push->_inDescriptor);

	push->_error = Error::success;
	push->complete();

	pull->_error = Error::success;
	pull->_descriptor = std::move(descriptor);
	pull->complete();
}

void Stream::Submitter::enqueue(const smarter::shared_ptr<Stream, LanePolicy> &lane, StreamList &chain) {
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
		auto s = u->_transmitLane.get();
		bool laneShutdown = false;
		bool laneBroken = false;
		{
			// p/q is the number of the local/remote lane.
			// u/v is the local/remote item that we are processing.
			int p = laneOf(u->_transmitLane);
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

		// First, handle the case that any action is "dismiss".
		// TODO: for now, we return kHelErrDismissed.
		//       In the future, we could return some out-of-band data by adding
		//       unions to the hel result structs (each struct has at least one
		//       unsigned int to spare in the error case).
		if(u->tag() == kTagDismiss || v->tag() == kTagDismiss) {
			Error uError{};
			Error vError{};
			if(u->tag() == kTagDismiss)
				vError = Error::dismissed;
			if(v->tag() == kTagDismiss)
				uError = Error::dismissed;
			s->_cancelItem(u, uError);
			s->_cancelItem(v, vError);
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
			auto branch = smarter::allocate_shared<Stream>(*kernelAlloc, CtorToken{});
			assert(branch.policy().base()->ctr().check_count() == 1);
			branch->selfPtr = branch;
			branch.policy().increment();
			branch.policy().increment();
			u->_lane = adoptLane(branch, 0);
			v->_lane = adoptLane(branch, 1);

			enqueue(u->_lane, u->ancillaryChain);
			enqueue(v->_lane, v->ancillaryChain);

			transfer(OfferAccept{}, u, v);
		}else if(u->tag() == kTagImbueCredentials && v->tag() == kTagExtractCredentials) {
			transfer(ImbueExtract{}, u, v);
		}else if(u->tag() == kTagSendKernelBuffer && v->tag() == kTagRecvKernelBuffer) {
			transfer(SendRecvInline{}, u, v);
		}else if(u->tag() == kTagSendFlow && v->tag() == kTagRecvKernelBuffer) {
			if(u->_inBuffer.size() > v->_maxLength) {
				// Both nodes complete with bufferTooSmall.
				u->_error = Error::bufferTooSmall;
				v->_error = Error::bufferTooSmall;

				u->issueFlow.raise();
				v->complete();
				continue;
			}else if(!u->_maxLength) {
				u->issueFlow.raise();
				v->complete();
				continue;
			}

			u->peerNode = v;
			u->issueFlow.raise();
		}else if(u->tag() == kTagSendKernelBuffer && v->tag() == kTagRecvFlow) {
			if(u->_inBuffer.size() > v->_maxLength) {
				// Both nodes complete with bufferTooSmall.
				u->_error = Error::bufferTooSmall;
				v->_error = Error::bufferTooSmall;

				u->complete();
				v->issueFlow.raise();
				continue;
			}else if(!u->_inBuffer.size()) {
				u->complete();
				v->issueFlow.raise();
				continue;
			}

			v->peerNode = u;
			v->issueFlow.raise();
		}else if(u->tag() == kTagSendFlow && v->tag() == kTagRecvFlow) {
			if(u->_maxLength > v->_maxLength) {
				// Both nodes complete with bufferTooSmall.
				u->_error = Error::bufferTooSmall;
				v->_error = Error::bufferTooSmall;

				u->issueFlow.raise();
				v->issueFlow.raise();
				continue;
			}else if(!u->_maxLength) {
				u->issueFlow.raise();
				v->issueFlow.raise();
				continue;
			}

			u->peerNode = v;
			u->issueFlow.raise();
			v->peerNode = u;
			v->issueFlow.raise();
		}else if(u->tag() == kTagPushDescriptor && v->tag() == kTagPullDescriptor) {
			transfer(PushPull{}, u, v);
		}else{
			u->_error = Error::transmissionMismatch;
			if(usesFlowProtocol(u->tag())) {
				u->issueFlow.raise();
			}else{
				u->complete();
			}

			v->_error = Error::transmissionMismatch;
			if(usesFlowProtocol(v->tag())) {
				v->issueFlow.raise();
			}else{
				v->complete();
			}
		}
	}
}

void Stream::onPeersZero(Stream *stream, int lane) {
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
}

Stream::Stream(CtorToken, bool withCredentials)
: _laneBroken{false, false}, _laneShutDown{false, false}, _withCredentials{withCredentials} {
	_peerCount[0].setup(smarter::adopt_rc, 1);
	_peerCount[1].setup(smarter::adopt_rc, 1);
	if(withCredentials)
		_creds = Credentials{};
}

Stream::~Stream() {
// TODO: remove debugging messages?
//	infoLogger() << "Closing stream" << frg::endlog;
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
	if(usesFlowProtocol(item->tag())) {
		item->issueFlow.raise();
	}else{
		item->complete();
	}

	while(!pending.empty()) {
		item = pending.pop_front();
		item->_error = error;
		if(usesFlowProtocol(item->tag())) {
			item->issueFlow.raise();
		}else{
			item->complete();
		}
	}
}

std::expected<
	frg::tuple<smarter::shared_ptr<Stream, LanePolicy>, smarter::shared_ptr<Stream, LanePolicy>>,
	Error
> createStream(bool withCredentials) {
	auto stream = smarter::allocate_shared<Stream>(*kernelAlloc, Stream::CtorToken{}, withCredentials);
	assert(stream.policy().base()->ctr().check_count() == 1);
	stream->selfPtr = stream;
	stream.policy().increment();
	auto handle1 = adoptLane(stream, 0);
	auto handle2 = adoptLane(stream, 1);
	stream.release();
	return frg::make_tuple(std::move(handle1), std::move(handle2));
}

} // namespace thor

