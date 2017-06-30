
#include "kernel.hpp"

namespace thor {

/*AsyncEvent AcceptPolicy::makeEvent(SubmitInfo info, Error error,
		frigg::WeakPtr<Universe> weak_universe, LaneDescriptor lane) {
	auto universe = weak_universe.grab();
	assert(universe);

	Handle handle;
	{
		Universe::Guard lock(&universe->lock);
		handle = universe->attachDescriptor(lock, frigg::move(lane));
	}

	AsyncEvent event(kEventAccept, info);
	event.error = error;
	event.handle = handle;
	return event;
}*/

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

static void transfer(frigg::SharedPtr<OfferBase> offer,
		frigg::SharedPtr<AcceptBase> accept, LaneDescriptor lane) {
	offer->complete(kErrSuccess);
	// TODO: move universe and lane?
	accept->complete(kErrSuccess, accept->_universe, frigg::move(lane));
}

static void transfer(frigg::SharedPtr<SendFromBufferBase> from,
		frigg::SharedPtr<RecvInlineBase> to) {
	auto buffer = frigg::move(from->buffer);
	from->complete(kErrSuccess);
	to->complete(kErrSuccess, frigg::move(buffer));
}

static void transfer(frigg::SharedPtr<SendFromBufferBase> from,
		frigg::SharedPtr<RecvToBufferBase> to) {
	if(from->buffer.size() <= to->accessor.length()) {
		to->accessor.copyTo(0, from->buffer.data(), from->buffer.size());
		from->complete(kErrSuccess);
		to->complete(kErrSuccess, from->buffer.size());
	}else{
		from->complete(kErrBufferTooSmall);
		to->complete(kErrBufferTooSmall, 0);
	}
}

static void transfer(frigg::SharedPtr<PushDescriptorBase> push,
		frigg::SharedPtr<PullDescriptorBase> pull) {
	push->complete(kErrSuccess);
	pull->complete(kErrSuccess, pull->_universe, push->_lane);
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
		auto lock = frigg::guard(&stream->_mutex);
		assert(!stream->_laneBroken[lane]);
		stream->_laneBroken[lane] = true;

		while(!stream->_processQueue[!lane].empty()) {
			auto item = stream->_processQueue[!lane].removeFront(); 

			if(OfferBase::classOf(*item)) {
				static_cast<OfferBase *>(item.get())->complete(kErrClosedRemotely);
			}else if(AcceptBase::classOf(*item)) {
				static_cast<AcceptBase *>(item.get())->complete(kErrClosedRemotely,
						frigg::WeakPtr<Universe>{}, LaneDescriptor{});
			}else if(SendFromBufferBase::classOf(*item)) {
				static_cast<SendFromBufferBase *>(item.get())->complete(kErrClosedRemotely);
			}else if(RecvToBufferBase::classOf(*item)) {
				static_cast<RecvToBufferBase *>(item.get())->complete(kErrClosedRemotely, 0);
			}else if(RecvInlineBase::classOf(*item)) {
				static_cast<RecvInlineBase *>(item.get())->complete(kErrClosedRemotely,
						frigg::UniqueMemory<KernelAlloc>{});
			}else if(PushDescriptorBase::classOf(*item)) {
				static_cast<PushDescriptorBase *>(item.get())->complete(kErrClosedRemotely);
			}else if(PullDescriptorBase::classOf(*item)) {
				static_cast<PullDescriptorBase *>(item.get())->complete(kErrClosedRemotely,
						frigg::WeakPtr<Universe>{}, AnyDescriptor{});
			}else{
				assert(!"Unexpected item in stream");
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

LaneHandle Stream::_submitControl(int p, frigg::SharedPtr<StreamControl> u) {
	// p/q is the number of the local/remote lane.
	// u/v is the local/remote item that we are processing.
	assert(!(p & ~int(1)));
	int q = 1 - p;
	frigg::SharedPtr<StreamControl> v;

	// the corresponding stream if u/v represent a conversation handshake.
	frigg::SharedPtr<Stream> conversation;

	// note: try to do as little work as possible while holding the lock.
	{
		auto lock = frigg::guard(&_mutex);
		assert(!_laneBroken[p]);
		if(!_processQueue[q].empty()) {
			v = _processQueue[q].removeFront();

			if(OfferBase::classOf(*v) || AcceptBase::classOf(*v))
				conversation = _conversationQueue.removeFront();
		}else if(_laneBroken[q]) {
			assert(!"Handle remotely broken lanes");
		}else{
			if(OfferBase::classOf(*u) || AcceptBase::classOf(*u)) {
				_processQueue[p].addBack(frigg::move(u));

				// initially there will be 3 references to the stream:
				// * one reference for the original shared pointer.
				// * one reference for each of the two lanes.
				conversation = frigg::makeShared<Stream>(*kernelAlloc);
				conversation.control().counter()->setRelaxed(3);
				
				// we will adopt exactly two LaneHandle objects per lane.
				conversation->_peerCount[0].store(2, std::memory_order_relaxed);
				conversation->_peerCount[1].store(2, std::memory_order_relaxed);

				LaneHandle handle(adoptLane, conversation, p);

				_conversationQueue.addBack(frigg::move(conversation));

				return frigg::move(handle);
			}else{
				_processQueue[p].addBack(frigg::move(u));

				return LaneHandle();
			}
		}
	}

	// do the main work here, after we released the lock.
	if(OfferBase::classOf(*u)
			&& AcceptBase::classOf(*v)) {
		LaneHandle lane1(adoptLane, conversation, p);
		LaneHandle lane2(adoptLane, conversation, q);

		transfer(frigg::staticPtrCast<OfferBase>(frigg::move(u)),
				frigg::staticPtrCast<AcceptBase>(frigg::move(v)),
				LaneDescriptor(frigg::move(lane2)));

		return LaneHandle(adoptLane, conversation, p);
	}else if(OfferBase::classOf(*v)
			&& AcceptBase::classOf(*u)) {
		LaneHandle lane1(adoptLane, conversation, p);
		LaneHandle lane2(adoptLane, conversation, q);

		transfer(frigg::staticPtrCast<OfferBase>(frigg::move(v)),
				frigg::staticPtrCast<AcceptBase>(frigg::move(u)),
				LaneDescriptor(frigg::move(lane1)));
		
		return LaneHandle(adoptLane, conversation, p);
	}else if(SendFromBufferBase::classOf(*u)
			&& RecvInlineBase::classOf(*v)) {
		transfer(frigg::staticPtrCast<SendFromBufferBase>(frigg::move(u)),
				frigg::staticPtrCast<RecvInlineBase>(frigg::move(v)));
		return LaneHandle();
	}else if(SendFromBufferBase::classOf(*v)
			&& RecvInlineBase::classOf(*u)) {
		transfer(frigg::staticPtrCast<SendFromBufferBase>(frigg::move(v)),
				frigg::staticPtrCast<RecvInlineBase>(frigg::move(u)));
		return LaneHandle();
	}else if(SendFromBufferBase::classOf(*u)
			&& RecvToBufferBase::classOf(*v)) {
		transfer(frigg::staticPtrCast<SendFromBufferBase>(frigg::move(u)),
				frigg::staticPtrCast<RecvToBufferBase>(frigg::move(v)));
		return LaneHandle();
	}else if(SendFromBufferBase::classOf(*v)
			&& RecvToBufferBase::classOf(*u)) {
		transfer(frigg::staticPtrCast<SendFromBufferBase>(frigg::move(v)),
				frigg::staticPtrCast<RecvToBufferBase>(frigg::move(u)));
		return LaneHandle();
	}else if(PushDescriptorBase::classOf(*u)
			&& PullDescriptorBase::classOf(*v)) {
		transfer(frigg::staticPtrCast<PushDescriptorBase>(frigg::move(u)),
				frigg::staticPtrCast<PullDescriptorBase>(frigg::move(v)));
		return LaneHandle();
	}else if(PushDescriptorBase::classOf(*v)
			&& PullDescriptorBase::classOf(*u)) {
		transfer(frigg::staticPtrCast<PushDescriptorBase>(frigg::move(v)),
				frigg::staticPtrCast<PullDescriptorBase>(frigg::move(u)));
		return LaneHandle();
	}else{
		frigg::infoLogger() << u->tag()
				<< " vs. " << v->tag() << frigg::endLog;
		assert(!"Operations do not match");
		__builtin_trap();
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

