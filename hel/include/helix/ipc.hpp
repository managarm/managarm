
#ifndef HELIX_HPP
#define HELIX_HPP

#include <assert.h>
#include <atomic>
#include <initializer_list>
#include <list>
#include <stdexcept>

#include <async/result.hpp>
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

struct OperationBase {
	friend class Dispatcher;

	OperationBase()
	: _asyncId(0), _element(nullptr) { }
	
	virtual ~OperationBase() { }

	void *element() {
		return _element;
	}

protected:
	int64_t _asyncId;
public: // TODO: This should not be public.
	void *_element;
};

struct Operation : OperationBase {
	virtual void parse(void *&element) {
		assert(!"Not supported");
	}
};

struct Context {
	virtual void complete(ElementPtr element) = 0;
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

				auto context = reinterpret_cast<Context *>(elem->context);
				context->complete(ElementPtr{ptr + sizeof(HelElement)});
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

struct ManageMemory : Operation {
	HelError error() {
		return result()->error;
	}

	uintptr_t offset() {
		return result()->offset;
	}

	size_t length() {
		return result()->length;
	}

private:
	HelManageResult *result() {
		return reinterpret_cast<HelManageResult *>(OperationBase::element());
	}
};

struct LockMemory : Operation {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

struct Offer : Operation {
	HelError error() {
		return result()->error;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

struct Accept : Operation {
	HelError error() {
		return result()->error;
	}
	
	UniqueDescriptor descriptor() {
		HEL_CHECK(error());
		UniqueDescriptor descriptor(result()->handle);
		result()->handle = kHelNullHandle;
		return descriptor;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelHandleResult);
	}

private:
	HelHandleResult *result() {
		return reinterpret_cast<HelHandleResult *>(OperationBase::element());
	}
};

struct RecvInline : Operation {
	HelError error() {
		return result()->error;
	}
	
	void *data() {
		HEL_CHECK(error());
		return result()->data;
	}

	size_t length() {
		HEL_CHECK(error());
		return result()->length;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelInlineResult)
				+ ((result()->length + 7) & ~size_t(7));
	}

private:
	HelInlineResult *result() {
		return reinterpret_cast<HelInlineResult *>(OperationBase::element());
	}
};

struct RecvBuffer : Operation {
	HelError error() {
		return result()->error;
	}
	
	size_t actualLength() {
		HEL_CHECK(error());
		return result()->length;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelLengthResult);
	}

private:
	HelLengthResult *result() {
		return reinterpret_cast<HelLengthResult *>(OperationBase::element());
	}
};

struct PullDescriptor : Operation {
	HelError error() {
		return result()->error;
	}
	
	UniqueDescriptor descriptor() {
		HEL_CHECK(error());
		UniqueDescriptor descriptor(result()->handle);
		result()->handle = kHelNullHandle;
		return descriptor;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelHandleResult);
	}

private:
	HelHandleResult *result() {
		return reinterpret_cast<HelHandleResult *>(OperationBase::element());
	}
};

struct SendBuffer : Operation {
	HelError error() {
		return result()->error;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

struct PushDescriptor : Operation {
	HelError error() {
		return result()->error;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

struct AwaitIrq : Operation {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

struct Observe : Operation {
	HelError error() {
		return result()->error;
	}

	int observation() {
		return result()->observation;
	}

private:
	HelObserveResult *result() {
		return reinterpret_cast<HelObserveResult *>(OperationBase::element());
	}
};

// ----------------------------------------------------------------------------
// Experimental: submitAsync
// ----------------------------------------------------------------------------

struct Submission : private Context {
	Submission(BorrowedDescriptor memory, ManageMemory *operation,
			Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitManageMemory(memory.getHandle(),
				dispatcher.acquire().get(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(BorrowedDescriptor memory, LockMemory *operation,
			uintptr_t offset, size_t size, Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitLockMemory(memory.getHandle(), offset, size,
				dispatcher.acquire().get(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(BorrowedDescriptor thread, Observe *operation,
			Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitObserve(thread.getHandle(),
				dispatcher.acquire().get(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(BorrowedDescriptor descriptor, AwaitIrq *operation,
			Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitWaitForIrq(descriptor.getHandle(),
				dispatcher.acquire().get(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(const Submission &) = delete;

	Submission &operator= (Submission &other) = delete;

	async::result<void> async_wait() {
		return _pledge.async_get();
	}

private:
	Context *context() {
		return this;
	}

	void complete(ElementPtr element) override {
		_result->_element = element.get();
		_pledge.set_value();
	}

	Operation *_result;
	async::promise<void> _pledge;
};

struct Item {
	Operation *operation;
	HelAction action;
};

inline Item action(Offer *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionOffer;
	action.flags = flags;
	return {operation, action};
}

inline Item action(Accept *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionAccept;
	action.flags = flags;
	return {operation, action};
}

inline Item action(SendBuffer *operation, const void *buffer, size_t length,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionSendFromBuffer;
	action.flags = flags;
	action.buffer = const_cast<void *>(buffer);
	action.length = length;
	return {operation, action};
}

inline Item action(RecvInline *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionRecvInline;
	action.flags = flags;
	return {operation, action};
}

inline Item action(RecvBuffer *operation, void *buffer, size_t length,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionRecvToBuffer;
	action.flags = flags;
	action.buffer = buffer;
	action.length = length;
	return {operation, action};
}

inline Item action(PushDescriptor *operation, BorrowedDescriptor descriptor,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionPushDescriptor;
	action.flags = flags;
	action.handle = descriptor.getHandle();
	return {operation, action};
}

inline Item action(PullDescriptor *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionPullDescriptor;
	action.flags = flags;
	return {operation, action};
}

template<size_t N>
struct Transmission : private Context {
	Transmission(BorrowedDescriptor descriptor, const Item *items, Dispatcher &dispatcher) {
		HelAction actions[N];
		for(size_t i = 0; i < N; i++) {
			actions[i] = items[i].action;
			_results[i] = items[i].operation;
		}

		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitAsync(descriptor.getHandle(), actions, N,
				dispatcher.acquire().get(),
				reinterpret_cast<uintptr_t>(context), 0));
	}

	Transmission(const Transmission &) = delete;

	Transmission &operator= (Transmission &other) = delete;

	async::result<void> async_wait() {
		return _pledge.async_get();
	}

private:
	void complete(ElementPtr element) override {
		auto ptr = element.get();
		for(size_t i = 0; i < N; ++i)
			_results[i]->parse(ptr);
		_pledge.set_value();
	}

	Operation *_results[N];
	async::promise<void> _pledge;
};

inline Submission submitManageMemory(BorrowedDescriptor memory, ManageMemory *operation,
		Dispatcher &dispatcher) {
	return {memory, operation, dispatcher};
}

inline Submission submitLockMemory(BorrowedDescriptor memory, LockMemory *operation,
		uintptr_t offset, size_t size, Dispatcher &dispatcher) {
	return {memory, operation, offset, size, dispatcher};
}

inline Submission submitObserve(BorrowedDescriptor thread, Observe *operation,
		Dispatcher &dispatcher) {
	return {thread, operation, dispatcher};
}

template<size_t N>
inline Transmission<N> submitAsync(BorrowedDescriptor descriptor, const Item (&items)[N],
		Dispatcher &dispatcher) {
	return {descriptor, items, dispatcher};
}

inline Submission submitAwaitIrq(BorrowedDescriptor descriptor, AwaitIrq *operation,
		Dispatcher &dispatcher) {
	return {descriptor, operation, dispatcher};
}

} // namespace helix

#endif // HELIX_HPP

