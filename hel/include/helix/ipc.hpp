
#ifndef HELIX_HPP
#define HELIX_HPP

#include <assert.h>
#include <atomic>
#include <initializer_list>
#include <stdexcept>

#include <hel.h>
#include <hel-syscalls.h>

namespace helix {

struct UniqueDescriptor {
	friend void swap(UniqueDescriptor &a, UniqueDescriptor &b) {
		using std::swap;
		swap(a._handle, b._handle);
	}

	UniqueDescriptor()
	: _handle(kHelNullHandle) { }
	
	UniqueDescriptor(const UniqueDescriptor &other) = delete;

	UniqueDescriptor(UniqueDescriptor &&other)
	: UniqueDescriptor() {
		swap(*this, other);
	}

	explicit UniqueDescriptor(HelHandle handle)
	: _handle(handle) { }

	~UniqueDescriptor() {
		if(_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(_handle));
	}

	UniqueDescriptor &operator= (UniqueDescriptor other) {
		swap(*this, other);
		return *this;
	}

	HelHandle getHandle() const {
		return _handle;
	}

	void release() {
		_handle = kHelNullHandle;
	}

private:
	HelHandle _handle;
};

struct BorrowedDescriptor {
	BorrowedDescriptor()
	: _handle(kHelNullHandle) { }
	
	BorrowedDescriptor(const BorrowedDescriptor &other) = default;
	BorrowedDescriptor(BorrowedDescriptor &&other) = default;

	explicit BorrowedDescriptor(HelHandle handle)
	: _handle(handle) { }
	
	BorrowedDescriptor(const UniqueDescriptor &other)
	: BorrowedDescriptor(other.getHandle()) { }

	~BorrowedDescriptor() = default;

	BorrowedDescriptor &operator= (const BorrowedDescriptor &) = default;

	HelHandle getHandle() const {
		return _handle;
	}

private:
	HelHandle _handle;
};

template<typename Tag>
struct UniqueResource : UniqueDescriptor {
	UniqueResource() = default;

	explicit UniqueResource(HelHandle handle)
	: UniqueDescriptor(handle) { }

	UniqueResource(UniqueDescriptor descriptor)
	: UniqueDescriptor(std::move(descriptor)) { }
};

template<typename Tag>
struct BorrowedResource : BorrowedDescriptor {
	BorrowedResource() = default;

	explicit BorrowedResource(HelHandle handle)
	: BorrowedDescriptor(handle) { }

	BorrowedResource(BorrowedDescriptor descriptor)
	: BorrowedDescriptor(descriptor) { }

	BorrowedResource(const UniqueResource<Tag> &other)
	: BorrowedDescriptor(other) { }

	UniqueResource<Tag> dup() {
		HelHandle new_handle;
		HEL_CHECK(helTransferDescriptor(getHandle(), kHelThisUniverse, &new_handle));
		return UniqueResource<Tag>(new_handle);
	}
};

struct Hub { };
using UniqueHub = UniqueResource<Hub>;
using BorrowedHub = BorrowedResource<Hub>;

inline UniqueHub createHub() {
	HelHandle handle;
	HEL_CHECK(helCreateEventHub(&handle));
	return UniqueHub(handle);
}

struct Pipe { };
using UniquePipe = UniqueResource<Pipe>;
using BorrowedPipe = BorrowedResource<Pipe>;

inline std::pair<UniquePipe, UniquePipe> createFullPipe() {
	HelHandle first_handle, second_handle;
	HEL_CHECK(helCreateFullPipe(&first_handle, &second_handle));
	return { UniquePipe(first_handle), UniquePipe(second_handle) };
}

struct Lane { };
using UniqueLane = UniqueResource<Lane>;
using BorrowedLane = BorrowedResource<Lane>;

inline std::pair<UniqueLane, UniqueLane> createStream() {
	HelHandle first_handle, second_handle;
	HEL_CHECK(helCreateStream(&first_handle, &second_handle));
	return { UniqueLane(first_handle), UniqueLane(second_handle) };
}

struct Irq { };
using UniqueIrq = UniqueResource<Irq>;
using BorrowedIrq = BorrowedResource<Irq>;

struct QueuePtr {
	explicit QueuePtr(HelQueue *queue)
	: _queue(queue) { }

	HelQueue *get() {
		return _queue;
	}

private:
	HelQueue *_queue;
};

struct ElementPtr {
	explicit ElementPtr(void *queue)
	: _queue(queue) { }

	void *get() {
		return _queue;
	}

private:
	void *_queue;
};

// we use a pattern similar to CRTP here to reduce code size.
struct OperationBase {
	friend class Dispatcher;

	OperationBase()
	: _asyncId(0), _result(nullptr) { }
	
	virtual ~OperationBase() { }

	void *element() {
		return _result.get();
	}

protected:
	virtual void complete() = 0;

	int64_t _asyncId;
	ElementPtr _result;
};

template<typename M>
struct Operation : OperationBase {
	typename M::Future future() {
		return _completer.future();
	}

protected:
	void complete() override final {
		_completer();
	}

private:
	typename M::Completer _completer;
};

struct Dispatcher {
	static Dispatcher &global();
	
	explicit Dispatcher()
	: _queue(nullptr), _progress(0) { }

	QueuePtr acquire() {
		if(!_queue) {
			auto ptr = operator new(sizeof(HelQueue) + 4096);
			_queue = reinterpret_cast<HelQueue *>(ptr);
			_queue->elementLimit = 128;
			_queue->queueLength = 4096;
			_queue->kernelState = 0;
			_queue->userState = 0;
		}
		return QueuePtr(_queue);
	}

	void dispatch() {
		assert(_queue);

		unsigned int e = __atomic_load_n(&_queue->kernelState, __ATOMIC_ACQUIRE);
		while(true) {
			if(_progress < (e & kHelQueueTail)) {
				auto ptr = (char *)_queue + sizeof(HelQueue) + _progress;
				auto elem = reinterpret_cast<HelElement *>(ptr);
				_progress += sizeof(HelElement) + elem->length;

				auto base = reinterpret_cast<OperationBase *>(elem->context);
				base->_result = ElementPtr(ptr + sizeof(HelElement));
				base->complete();
				break;
			}

			if(!(e & kHelQueueWaiters)) {
				auto d = e | kHelQueueWaiters;
				if(__atomic_compare_exchange_n(&_queue->kernelState,
						&e, d, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
					e = d;
			}else{
				HEL_CHECK(helFutexWait((int *)&_queue->kernelState, e));
				e = __atomic_load_n(&_queue->kernelState, __ATOMIC_ACQUIRE);
			}
		}
	}

private:
	HelQueue *_queue;
	size_t _progress;
};

template<typename M>
struct Offer : Operation<M> {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

template<typename M>
struct Accept : Operation<M> {
	HelError error() {
		return result()->error;
	}
	
	UniqueDescriptor descriptor() {
		HEL_CHECK(error());
		UniqueDescriptor descriptor(result()->handle);
		result()->handle = kHelNullHandle;
		return descriptor;
	}

private:
	HelHandleResult *result() {
		return reinterpret_cast<HelHandleResult *>(OperationBase::element());
	}
};

template<typename M>
struct RecvBuffer : Operation<M> {
	HelError error() {
		return result()->error;
	}
	
	size_t actualLength() {
		HEL_CHECK(error());
		return result()->length;
	}

private:
	HelLengthResult *result() {
		return reinterpret_cast<HelLengthResult *>(OperationBase::element());
	}
};

template<typename M>
struct PullDescriptor : Operation<M> {
	HelError error() {
		return result()->error;
	}
	
	UniqueDescriptor descriptor() {
		HEL_CHECK(error());
		UniqueDescriptor descriptor(result()->handle);
		result()->handle = kHelNullHandle;
		return descriptor;
	}

private:
	HelHandleResult *result() {
		return reinterpret_cast<HelHandleResult *>(OperationBase::element());
	}
};

template<typename M>
struct SendBuffer : Operation<M> {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

template<typename M>
struct PushDescriptor : Operation<M> {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

template<typename M>
struct AwaitIrq : Operation<M> {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

// ----------------------------------------------------------------------------
// Experimental: submitAsync
// ----------------------------------------------------------------------------

template<typename M>
HelAction action(Offer<M> *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionOffer;
	action.context = (uintptr_t)operation;
	action.flags = flags;
	return action;
}

template<typename M>
HelAction action(Accept<M> *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionAccept;
	action.context = (uintptr_t)operation;
	action.flags = flags;
	return action;
}

template<typename M>
HelAction action(SendBuffer<M> *operation, const void *buffer, size_t length,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionSendFromBuffer;
	action.context = (uintptr_t)operation;
	action.flags = flags;
	action.buffer = const_cast<void *>(buffer);
	action.length = length;
	return action;
}

template<typename M>
HelAction action(RecvBuffer<M> *operation, void *buffer, size_t length,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionRecvToBuffer;
	action.context = (uintptr_t)operation;
	action.flags = flags;
	action.buffer = buffer;
	action.length = length;
	return action;
}

template<typename M>
HelAction action(PushDescriptor<M> *operation, BorrowedDescriptor descriptor,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionPushDescriptor;
	action.context = (uintptr_t)operation;
	action.flags = flags;
	action.handle = descriptor.getHandle();
	return action;
}

template<typename M>
HelAction action(PullDescriptor<M> *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionPullDescriptor;
	action.context = (uintptr_t)operation;
	action.flags = flags;
	return action;
}

template<size_t N>
void submitAsync(BorrowedDescriptor descriptor, const HelAction (&actions)[N],
		Dispatcher &dispatcher) {
	HEL_CHECK(helSubmitAsync(descriptor.getHandle(), actions, N,
			dispatcher.acquire().get(), 0));
}

} // namespace helix

#endif // HELIX_HPP

