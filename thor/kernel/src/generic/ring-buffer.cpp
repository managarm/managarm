
#include "kernel.hpp"

namespace thor {

AsyncRingItem::AsyncRingItem(AsyncCompleter completer,
		DirectSpaceAccessor<HelRingBuffer> space_lock, size_t buffer_size)
: AsyncOperation(frigg::move(completer)),
		spaceLock(frigg::move(space_lock)), bufferSize(buffer_size), offset(0) { }

RingBuffer::RingBuffer() { }

// TODO: protect this with a lock
void RingBuffer::submitBuffer(frigg::SharedPtr<AsyncRingItem> item) {
	_bufferQueue.addBack(frigg::move(item));
}

// TODO: protect this with a lock
void RingBuffer::doTransfer(frigg::SharedPtr<AsyncSendString> send,
		frigg::SharedPtr<AsyncRecvString> recv) {
	assert(!_bufferQueue.empty());

	AsyncRingItem &front = *_bufferQueue.front();

	if(front.offset + send->kernelBuffer.size() <= front.bufferSize) {
		size_t offset = front.offset;
		front.offset += send->kernelBuffer.size();

		__atomic_add_fetch(&front.spaceLock->refCount, 1, __ATOMIC_RELEASE);

		frigg::UnsafePtr<AddressSpace> space = front.spaceLock.space();
		auto address = (char *)front.spaceLock.foreignAddress() + sizeof(HelRingBuffer) + offset;
		auto data_lock = ForeignSpaceAccessor::acquire(space.toShared(), address,
				send->kernelBuffer.size());
		data_lock.copyTo(0, send->kernelBuffer.data(), send->kernelBuffer.size());

		send->error = kErrSuccess;

		recv->error = kErrSuccess;
		recv->msgRequest = send->msgRequest;
		recv->msgSequence = send->msgSequence;
		recv->offset = offset;
		recv->length = send->kernelBuffer.size();

		AsyncOperation::complete(frigg::move(send));
		AsyncOperation::complete(frigg::move(recv));
	}else{
		assert(!"TODO: Return the buffer to user-space");
	}
}

};

