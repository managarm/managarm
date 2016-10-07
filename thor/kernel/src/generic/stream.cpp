
#include "kernel.hpp"

namespace thor {

AsyncEvent AcceptPolicy::makeEvent(SubmitInfo info, Error error,
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
}

AsyncEvent PullDescriptorPolicy::makeEvent(SubmitInfo info, Error error,
		frigg::WeakPtr<Universe> weak_universe, AnyDescriptor lane) {
	auto universe = weak_universe.grab();
	assert(universe);

	Handle handle;
	{
		Universe::Guard lock(&universe->lock);
		handle = universe->attachDescriptor(lock, frigg::move(lane));
	}

	AsyncEvent event(kEventRecvDescriptor, info);
	event.error = error;
	event.handle = handle;
	return event;
}

void LaneDescriptor::submit(frigg::SharedPtr<StreamControl> control) {
	_handle.getStream()->submit(_handle.getLane(), frigg::move(control));
}

static void transfer(frigg::SharedPtr<OfferBase> offer,
		frigg::SharedPtr<AcceptBase> accept) {
	offer->complete(kErrSuccess);
	// TODO: move universe and lane?
	accept->complete(kErrSuccess, accept->_universe, offer->_lane);
}

static void transfer(frigg::SharedPtr<SendFromBufferBase> from,
		frigg::SharedPtr<RecvToBufferBase> to) {
	assert(from->buffer.size() <= to->accessor.length());
	to->accessor.copyTo(from->buffer.data(), from->buffer.size());
	from->complete(kErrSuccess);
	to->complete(kErrSuccess, from->buffer.size());
}

static void transfer(frigg::SharedPtr<PushDescriptorBase> push,
		frigg::SharedPtr<PullDescriptorBase> pull) {
	push->complete(kErrSuccess);
	pull->complete(kErrSuccess, pull->_universe, push->_lane);
}

Stream::Stream()
: peerCounter{1, 1} { }

void Stream::submit(int local, frigg::SharedPtr<StreamControl> control) {
	assert(!(local & ~int(1)));
	int remote = 1 - local;

	_processQueue[local].addBack(frigg::move(control));

	while(!_processQueue[local].empty()
			&& !_processQueue[remote].empty()) {
		auto local_item = _processQueue[local].removeFront();
		auto remote_item = _processQueue[remote].removeFront();

		if(OfferBase::classOf(*local_item)
				&& AcceptBase::classOf(*remote_item)) {
			transfer(frigg::staticPtrCast<OfferBase>(frigg::move(local_item)),
					frigg::staticPtrCast<AcceptBase>(frigg::move(remote_item)));
		}else if(OfferBase::classOf(*remote_item)
				&& AcceptBase::classOf(*local_item)) {
			transfer(frigg::staticPtrCast<OfferBase>(frigg::move(remote_item)),
					frigg::staticPtrCast<AcceptBase>(frigg::move(local_item)));
		}else if(SendFromBufferBase::classOf(*local_item)
				&& RecvToBufferBase::classOf(*remote_item)) {
			transfer(frigg::staticPtrCast<SendFromBufferBase>(frigg::move(local_item)),
					frigg::staticPtrCast<RecvToBufferBase>(frigg::move(remote_item)));
		}else if(SendFromBufferBase::classOf(*remote_item)
				&& RecvToBufferBase::classOf(*local_item)) {
			transfer(frigg::staticPtrCast<SendFromBufferBase>(frigg::move(remote_item)),
					frigg::staticPtrCast<RecvToBufferBase>(frigg::move(local_item)));
		}else if(PushDescriptorBase::classOf(*local_item)
				&& PullDescriptorBase::classOf(*remote_item)) {
			transfer(frigg::staticPtrCast<PushDescriptorBase>(frigg::move(local_item)),
					frigg::staticPtrCast<PullDescriptorBase>(frigg::move(remote_item)));
		}else{
			frigg::infoLogger() << local_item->tag()
					<< " vs. " << remote_item->tag() << frigg::endLog;
			assert(!"Operations do not match");
		}
	}
}

} // namespace thor

