
#ifndef HELIX_HPP
#define HELIX_HPP

#include <assert.h>
#include <atomic>
#include <initializer_list>
#include <list>
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
private:
	struct Item {
		Item(HelQueue *queue)
		: queue(queue), progress(0) { }

		Item(const Item &other) = delete;

		Item &operator= (const Item &other) = delete;

		HelQueue *queue;

		size_t progress;
	};

public:
	static Dispatcher &global();

	Dispatcher() = default;

	Dispatcher(const Dispatcher &) = delete;
	
	Dispatcher &operator= (const Dispatcher &) = delete;

	QueuePtr acquire() {
		if(_items.empty())
			allocate();
		return QueuePtr(_items.front().queue);
	}

	void dispatch() {
		assert(!_items.empty());

		auto it = std::prev(_items.end());

		auto ke = __atomic_load_n(&it->queue->kernelState, __ATOMIC_ACQUIRE);
		while(true) {
			if(it->progress != (ke & kHelQueueTail)) {
				assert(it->progress < (ke & kHelQueueTail));

				auto ptr = (char *)it->queue + sizeof(HelQueue) + it->progress;
				auto elem = reinterpret_cast<HelElement *>(ptr);
				it->progress += sizeof(HelElement) + elem->length;

				auto base = reinterpret_cast<OperationBase *>(elem->context);
				base->_result = ElementPtr(ptr + sizeof(HelElement));
				base->complete();
				break;
			}
			
			auto ue = __atomic_load_n(&it->queue->userState, __ATOMIC_RELAXED);
			if((ke & kHelQueueWantNext) && !(ue & kHelQueueHasNext)) {
				allocate();
				++it;
				assert(it == std::prev(_items.end()));
				ke = __atomic_load_n(&it->queue->kernelState, __ATOMIC_ACQUIRE);
				continue;
			}

			if(!(ke & kHelQueueWaiters)) {
				auto d = ke | kHelQueueWaiters;
				if(__atomic_compare_exchange_n(&it->queue->kernelState,
						&ke, d, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
					ke = d;
			}else{
				HEL_CHECK(helFutexWait((int *)&it->queue->kernelState, ke));
				ke = __atomic_load_n(&it->queue->kernelState, __ATOMIC_ACQUIRE);
			}
		}
	}

private:
	void allocate() {
		auto queue = reinterpret_cast<HelQueue *>(operator new(sizeof(HelQueue) + 0x1000));
		queue->elementLimit = 128;
		queue->queueLength = 0x1000;
		queue->kernelState = 0;
		queue->userState = 0;

		if(!_items.empty()) {
			_items.back().queue->nextQueue = queue;

			unsigned int e = 0;
			auto s = __atomic_compare_exchange_n(&_items.back().queue->userState,
					&e, kHelQueueHasNext, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
			assert(s);
			HEL_CHECK(helFutexWake((int *)&_items.back().queue->userState));
		}

		_items.emplace_back(queue);
	}

	std::list<Item> _items;
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

