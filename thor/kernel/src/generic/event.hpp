
#include <atomic>

namespace thor {
	
enum EventType {
	kEventNone,
	kEventMemoryLoad,
	kEventMemoryLock,
	kEventObserve,
	kEventOffer,
	kEventAccept,
	kEventSendString,
	kEventSendDescriptor,
	kEventRecvString,
	kEventRecvStringToRing,
	kEventRecvDescriptor,
	kEventIrq
};

struct AsyncEvent {
	AsyncEvent();

	AsyncEvent(EventType type, SubmitInfo submit_info);

	EventType type;
	SubmitInfo submitInfo;

	// used by kEventRecvStringError
	Error error;

	// used by kEventMemoryLoad, kEventRecvStringTransferToBuffer
	// and kEventRecvStringTransferToQueue
	size_t offset;
	size_t length;
	
	// used by kEventRecvStringTransferToBuffer, kEventRecvStringTransferToQueue
	// and kEventRecvDescriptor
	int64_t msgRequest;
	int64_t msgSequence;

	// used by kEventRecvDescriptor, kEventAccept, kEventConnect
	Handle handle;
};

typedef frigg::Variant<
	NullCompleter,
	PostEventCompleter,
	ReturnFromForkCompleter
> AsyncCompleter;

struct AsyncOperation {
	static void complete(frigg::SharedPtr<AsyncOperation> operation);

	AsyncOperation(AsyncCompleter completer)
	: completer(frigg::move(completer)), isComplete(false) { }

	virtual AsyncEvent getEvent() = 0;

	AsyncCompleter completer;
	
	std::atomic<bool> isComplete;
	
	frigg::IntrusiveSharedLinkedItem<AsyncOperation> hubItem;
};

template<typename P>
struct PostEvent {
	struct Item : AsyncOperation {
		Item(frigg::SharedPtr<EventHub> hub, uintptr_t context)
		: AsyncOperation(PostEventCompleter(frigg::move(hub), 0, 0, context)) { }

		AsyncEvent getEvent() override {
			return event;
		}

		AsyncEvent event;
	};

	struct Completer {
		explicit Completer(PostEvent token)
		: _item(frigg::makeShared<Item>(*kernelAlloc,
				frigg::move(token._hub), token._context)) { }

		template<typename... Args>
		void operator() (Args &&... args) {
			auto info = _item->completer.template get<PostEventCompleter>().submitInfo;
			_item->event = P::makeEvent(info, frigg::forward<Args>(args)...);
			AsyncOperation::complete(frigg::move(_item));
		}

	private:
		frigg::SharedPtr<Item> _item;
	};

	PostEvent(frigg::SharedPtr<EventHub> hub, uintptr_t context)
	: _hub(frigg::move(hub)), _context(context) { }

private:
	frigg::SharedPtr<EventHub> _hub;
	uintptr_t _context;
};

struct OfferPolicy {
	static AsyncEvent makeEvent(SubmitInfo info, Error error) {
		AsyncEvent event(kEventOffer, info);
		event.error = error;
		return event;
	}
};

struct AcceptPolicy {
	static AsyncEvent makeEvent(SubmitInfo info, Error error,
			frigg::WeakPtr<Universe> weak_universe, LaneDescriptor lane);
};

struct SendStringPolicy {
	static AsyncEvent makeEvent(SubmitInfo info, Error error) {
		AsyncEvent event(kEventSendString, info);
		event.error = error;
		return event;
	}
};

struct RecvStringPolicy {
	static AsyncEvent makeEvent(SubmitInfo info, Error error, size_t length) {
		AsyncEvent event(kEventRecvString, info);
		event.error = error;
		event.length = length;
		return event;
	}
};

struct PushDescriptorPolicy {
	static AsyncEvent makeEvent(SubmitInfo info, Error error) {
		AsyncEvent event(kEventSendDescriptor, info);
		event.error = error;
		return event;
	}
};

struct PullDescriptorPolicy {
	static AsyncEvent makeEvent(SubmitInfo info, Error error,
			frigg::WeakPtr<Universe> weak_universe, AnyDescriptor lane);
};

struct AsyncHandleLoad : public AsyncOperation {
	AsyncHandleLoad(AsyncCompleter completer)
	: AsyncOperation(frigg::move(completer)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncHandleLoad> processQueueItem;

	size_t offset;
	size_t length;
};

struct AsyncInitiateLoad : public AsyncOperation {
	AsyncInitiateLoad(AsyncCompleter completer, size_t offset, size_t length)
	: AsyncOperation(frigg::move(completer)), offset(offset), length(length), progress(0) { }

	size_t offset;
	size_t length;

	// byte offset for which AsyncHandleLoads have already been issued
	size_t progress;
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncInitiateLoad> processQueueItem;
};

struct AsyncObserve : public AsyncOperation {
	AsyncObserve(AsyncCompleter completer)
	: AsyncOperation(frigg::move(completer)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::IntrusiveSharedLinkedItem<AsyncObserve> processQueueItem;
};

struct AsyncWaitForEvent : public AsyncOperation {
	AsyncWaitForEvent(AsyncCompleter completer, int64_t filter_async_id)
	: AsyncOperation(frigg::move(completer)), filterAsyncId(filter_async_id) { }
	
	AsyncEvent getEvent() override;

	int64_t filterAsyncId;
	
	frigg::IntrusiveSharedLinkedItem<AsyncWaitForEvent> processQueueItem;

	AsyncEvent event;
};

struct AsyncSendString : public AsyncOperation {
	AsyncSendString(AsyncCompleter completer, int64_t msg_request, int64_t msg_sequence)
	: AsyncOperation(frigg::move(completer)), msgRequest(msg_request), msgSequence(msg_sequence),
			flags(0) { }
	
	AsyncEvent getEvent() override;
	
	frigg::UniqueMemory<KernelAlloc> kernelBuffer;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;

	frigg::IntrusiveSharedLinkedItem<AsyncSendString> processQueueItem;
	
	Error error;
};

struct AsyncSendDescriptor : public AsyncOperation {
	AsyncSendDescriptor(AsyncCompleter completer, int64_t msg_request, int64_t msg_sequence)
	: AsyncOperation(frigg::move(completer)), msgRequest(msg_request), msgSequence(msg_sequence),
			flags(0) { }
	
	AsyncEvent getEvent() override;
	
	AnyDescriptor descriptor;
	int64_t msgRequest;
	int64_t msgSequence;
	uint32_t flags;

	frigg::IntrusiveSharedLinkedItem<AsyncSendDescriptor> processQueueItem;
	
	Error error;
};

struct AsyncRecvString : public AsyncOperation {
	enum Type {
		kTypeNormal,
		kTypeToRing
	};

	AsyncRecvString(AsyncCompleter completer, Type type,
			int64_t filter_request, int64_t filter_sequence)
	: AsyncOperation(frigg::move(completer)), type(type),
			filterRequest(filter_request), filterSequence(filter_sequence) { }
	
	AsyncEvent getEvent() override;
	
	Type type;
	int64_t filterRequest;
	int64_t filterSequence;
	uint32_t flags;
	
	// used by kTypeNormal
	ForeignSpaceAccessor spaceLock;
	
	// used by kTypeToRing
	frigg::SharedPtr<RingBuffer> ringBuffer;
	
	frigg::IntrusiveSharedLinkedItem<AsyncRecvString> processQueueItem;

	Error error;
	int64_t msgRequest;
	int64_t msgSequence;
	size_t offset;
	size_t length;
};

struct AsyncRecvDescriptor : public AsyncOperation {
	AsyncRecvDescriptor(AsyncCompleter completer, frigg::WeakPtr<Universe> universe,
			int64_t filter_request, int64_t filter_sequence)
	: AsyncOperation(frigg::move(completer)), universe(frigg::move(universe)),
			filterRequest(filter_request), filterSequence(filter_sequence) { }
	
	AsyncEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	int64_t filterRequest;
	int64_t filterSequence;
	uint32_t flags;
	
	frigg::IntrusiveSharedLinkedItem<AsyncRecvDescriptor> processQueueItem;

	Error error;
	int64_t msgRequest;
	int64_t msgSequence;
	Handle handle;
};

struct AsyncRingItem : public AsyncOperation {
	AsyncRingItem(AsyncCompleter completer, DirectSpaceAccessor<HelRingBuffer> space_lock,
			size_t buffer_size);
	
	AsyncEvent getEvent() override;

	DirectSpaceAccessor<HelRingBuffer> spaceLock;
	size_t bufferSize;

	size_t offset;

	frigg::IntrusiveSharedLinkedItem<AsyncRingItem> bufferItem;
};

struct AsyncIrq : public AsyncOperation {
	AsyncIrq(AsyncCompleter completer)
	: AsyncOperation(frigg::move(completer)) { }
	
	AsyncEvent getEvent() override;
	
	frigg::WeakPtr<Universe> universe;
	
	frigg::IntrusiveSharedLinkedItem<AsyncIrq> processQueueItem;
};

class EventHub {
public:
	typedef frigg::TicketLock Lock;
	typedef frigg::LockGuard<Lock> Guard;

	EventHub();

	void raiseEvent(Guard &guard, frigg::SharedPtr<AsyncOperation> operation);

	void submitWaitForEvent(Guard &guard, frigg::SharedPtr<AsyncWaitForEvent> wait);

	Lock lock;

private:	
	frigg::IntrusiveSharedLinkedList<
		AsyncOperation,
		&AsyncOperation::hubItem
	> _eventQueue;

	frigg::IntrusiveSharedLinkedList<
		AsyncWaitForEvent,
		&AsyncWaitForEvent::processQueueItem
	> _waitQueue;
};

} // namespace thor

