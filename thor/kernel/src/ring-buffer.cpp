
#include "kernel.hpp"

namespace thor {

AsyncOperation::AsyncOperation(frigg::WeakPtr<EventHub> event_hub,
		SubmitInfo submit_info)
: eventHub(frigg::move(event_hub)), submitInfo(submit_info) { }

AsyncRingItem::AsyncRingItem(frigg::WeakPtr<EventHub> event_hub,
		SubmitInfo submit_info, DirectSpaceLock<HelRingBuffer> space_lock,
		size_t buffer_size)
: AsyncOperation(frigg::move(event_hub), submit_info),
		spaceLock(frigg::move(space_lock)), bufferSize(buffer_size), offset(0) { }

RingBuffer::RingBuffer()
: _items(*kernelAlloc) { }

// TODO: protect this with a lock
void RingBuffer::submitBuffer(AsyncRingItem item) {
	_items.addBack(frigg::move(item));
}

// TODO: protect this with a lock
void RingBuffer::doTransfer(AsyncSendString send, AsyncRecvString recv) {
	assert(!_items.empty());

	AsyncRingItem &front = _items.front();

	if(front.offset + send.kernelBuffer.size() <= front.bufferSize) {
		size_t offset = front.offset;
		front.offset += send.kernelBuffer.size();

		__atomic_add_fetch(&front.spaceLock->refCount, 1, __ATOMIC_RELEASE);

		frigg::SharedPtr<AddressSpace> space(front.spaceLock.space());
		auto address = (char *)front.spaceLock.foreignAddress() + sizeof(HelRingBuffer) + offset;
		auto data_lock = ForeignSpaceLock::acquire(frigg::move(space), address,
				send.kernelBuffer.size());
		data_lock.copyTo(send.kernelBuffer.data(), send.kernelBuffer.size());

		// post the receive event
		UserEvent event(UserEvent::kTypeRecvStringTransferToQueue, recv.submitInfo);
		event.length = send.kernelBuffer.size();
		event.offset = offset;
		event.msgRequest = send.msgRequest;
		event.msgSequence = send.msgSequence;
	
		frigg::SharedPtr<EventHub> recv_hub(recv.eventHub);
		EventHub::Guard recv_guard(&recv_hub->lock);
		recv_hub->raiseEvent(recv_guard, frigg::move(event));
		recv_guard.unlock();
	}else{
		assert(!"TODO: Return the buffer to user-space");
	}
}

};

