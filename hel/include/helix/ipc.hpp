
#ifndef HELIX_HPP
#define HELIX_HPP

#include <assert.h>
#include <atomic>
#include <initializer_list>
#include <list>
#include <tuple>
#include <array>
#include <stdexcept>

#include <async/result.hpp>

// This is here since ipc-structs.hpp needs ElementHandle
namespace helix {

struct Dispatcher;

struct ElementHandle {
	friend void swap(ElementHandle &u, ElementHandle &v) {
		using std::swap;
		swap(u._dispatcher, v._dispatcher);
		swap(u._cn, v._cn);
		swap(u._data, v._data);
	}

	ElementHandle()
	: _dispatcher{nullptr}, _cn{-1}, _data{nullptr} { }

	explicit ElementHandle(Dispatcher *dispatcher, int cn, void *data)
	: _dispatcher{dispatcher}, _cn{cn}, _data{data} { }

	ElementHandle(const ElementHandle &other);

	ElementHandle(ElementHandle &&other)
	: ElementHandle{} {
		swap(*this, other);
	}

	~ElementHandle();

	ElementHandle &operator= (ElementHandle other) {
		swap(*this, other);
		return *this;
	}

	void *data() {
		return _data;
	}

private:
	Dispatcher *_dispatcher;
	int _cn;
	void *_data;
};

} // namespace helix

#include "ipc-structs.hpp"

namespace helix {

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

	UniqueResource<Tag> dup() const {
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

struct OperationBase {
	friend struct Dispatcher;

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
	Operation()
	: _asyncId{0} { }

	uint64_t asyncId() {
		return _asyncId;
	}

	void setAsyncId(uint64_t async_id) {
		_asyncId = async_id;
	}

	virtual void parse(void *&element) {
		assert(!"Not supported");
	}

private:
	uint64_t _asyncId;
};

struct Context {
	virtual ~Context() = default;

	virtual void complete(ElementHandle element) = 0;
};

struct CurrentDispatcherToken {
	void wait();
};

inline constexpr CurrentDispatcherToken currentDispatcher;

struct Dispatcher {
	friend struct ElementHandle;

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
	static constexpr int sizeShift = 9;

	static Dispatcher &global();

	Dispatcher()
	: _handle{kHelNullHandle}, _queue{nullptr},
			_activeChunks{0}, _retrieveIndex{0}, _nextIndex{0}, _lastProgress{0} { }

	Dispatcher(const Dispatcher &) = delete;

	Dispatcher &operator= (const Dispatcher &) = delete;

	HelHandle acquire() {
		if(!_handle) {
			_queue = reinterpret_cast<HelQueue *>(operator new(sizeof(HelQueue)
					+ (1 << sizeShift) * sizeof(int)));
			_queue->headFutex = 0;
			HEL_CHECK(helCreateQueue(_queue, 0, sizeShift, 128, &_handle));
		}

		return _handle;
	}

	void wait() {
		while(true) {
			if(_retrieveIndex == _nextIndex) {
				assert(_activeChunks < (1 << sizeShift));
				if(_activeChunks >= 16)
					std::cerr << "\e[35mhelix: Queue is forced to grow to " << _activeChunks
							<< " chunks (memory leak?)\e[39m" << std::endl;

				auto chunk = reinterpret_cast<HelChunk *>(operator new(sizeof(HelChunk) + 4096));
				_chunks[_activeChunks] = chunk;
				HEL_CHECK(helSetupChunk(_handle, _activeChunks, chunk, 0));

				// Reset and enqueue the new chunk.
				chunk->progressFutex = 0;

				_queue->indexQueue[_nextIndex & ((1 << sizeShift) - 1)] = _activeChunks;
				_nextIndex = ((_nextIndex + 1) & kHelHeadMask);
				_wakeHeadFutex();

				_refCounts[_activeChunks] = 1;
				_activeChunks++;
				continue;
			}else if (_hadWaiters && _activeChunks < (1 << sizeShift)) {
//				std::cerr << "\e[35mhelix: Growing queue to " << _activeChunks
//						<< " chunks to improve throughput\e[39m" << std::endl;

				auto chunk = reinterpret_cast<HelChunk *>(operator new(sizeof(HelChunk) + 4096));
				_chunks[_activeChunks] = chunk;
				HEL_CHECK(helSetupChunk(_handle, _activeChunks, chunk, 0));

				// Reset and enqueue the new chunk.
				chunk->progressFutex = 0;

				_queue->indexQueue[_nextIndex & ((1 << sizeShift) - 1)] = _activeChunks;
				_nextIndex = ((_nextIndex + 1) & kHelHeadMask);
				_wakeHeadFutex();

				_refCounts[_activeChunks] = 1;
				_activeChunks++;
				_hadWaiters = false;
			}

			bool done;
			_waitProgressFutex(&done);
			if(done) {
				_surrender(_numberOf(_retrieveIndex));

				_lastProgress = 0;
				_retrieveIndex = ((_retrieveIndex + 1) & kHelHeadMask);
				continue;
			}

			// Dequeue the next element.
			auto ptr = (char *)_retrieveChunk() + sizeof(HelChunk) + _lastProgress;
			auto element = reinterpret_cast<HelElement *>(ptr);
			_lastProgress += sizeof(HelElement) + element->length;

			auto context = reinterpret_cast<Context *>(element->context);
			_refCounts[_numberOf(_retrieveIndex)]++;
			context->complete(ElementHandle{this, _numberOf(_retrieveIndex),
					ptr + sizeof(HelElement)});
			return;
		}
	}

private:
	void _surrender(int cn) {
		assert(_refCounts[cn] > 0);
		if(_refCounts[cn]-- > 1)
			return;

		// Reset and requeue the chunk.
		_chunks[cn]->progressFutex = 0;

		_queue->indexQueue[_nextIndex & ((1 << sizeShift) - 1)] = cn;
		_nextIndex = ((_nextIndex + 1) & kHelHeadMask);
		_wakeHeadFutex();

		_refCounts[cn] = 1;
	}

	void _reference(int cn) {
		_refCounts[cn]++;
	}

private:
	int _numberOf(int index) {
		return _queue->indexQueue[index & ((1 << sizeShift) - 1)];
	}

	HelChunk *_retrieveChunk() {
		auto cn = _queue->indexQueue[_retrieveIndex & ((1 << sizeShift) - 1)];
		return _chunks[cn];
	}

	void _wakeHeadFutex() {
		auto futex = __atomic_exchange_n(&_queue->headFutex, _nextIndex, __ATOMIC_RELEASE);
		if(futex & kHelHeadWaiters) {
			HEL_CHECK(helFutexWake(&_queue->headFutex));
			_hadWaiters = true;
		}
	}

	void _waitProgressFutex(bool *done) {
		while(true) {
			auto futex = __atomic_load_n(&_retrieveChunk()->progressFutex, __ATOMIC_ACQUIRE);
			do {
				if(_lastProgress != (futex & kHelProgressMask)) {
					*done = false;
					return;
				}else if(futex & kHelProgressDone) {
					*done = true;
					return;
				}

				assert(futex == _lastProgress);
			} while(!__atomic_compare_exchange_n(&_retrieveChunk()->progressFutex, &futex,
						_lastProgress | kHelProgressWaiters,
						false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));

			HEL_CHECK(helFutexWait(&_retrieveChunk()->progressFutex,
					_lastProgress | kHelProgressWaiters, -1));
		}
	}

private:
	HelHandle _handle;
	HelQueue *_queue;
	HelChunk *_chunks[1 << sizeShift];

	int _activeChunks;
	bool _hadWaiters;

	// Index of the chunk that we are currently retrieving/inserting next.
	int _retrieveIndex;
	int _nextIndex;

	// Progress into the current chunk.
	int _lastProgress;

	// Per-chunk reference counts.
	int _refCounts[1 << sizeShift];
};

async::run_queue *globalQueue();

inline void CurrentDispatcherToken::wait() {
	async::queue_scope qs{globalQueue()};
	Dispatcher::global().wait();
}

inline ElementHandle::~ElementHandle() {
	if(_dispatcher)
		_dispatcher->_surrender(_cn);
}

inline ElementHandle::ElementHandle(const ElementHandle &other) {
	_dispatcher = other._dispatcher;
	_cn = other._cn;
	_data = other._data;

	_dispatcher->_reference(_cn);
}

struct AwaitClock : Operation {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

struct ProtectMemory : Operation {
	HelError error() {
		return result()->error;
	}

private:
	HelSimpleResult *result() {
		return reinterpret_cast<HelSimpleResult *>(OperationBase::element());
	}
};

struct ManageMemory : Operation {
	HelError error() {
		return result()->error;
	}

	int type() {
		return result()->type;
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

struct LockMemoryView : Operation {
	static void completeOperation(Operation *base) {
		auto self = static_cast<LockMemoryView *>(base);
		if(!self->error())
			self->_descriptor = UniqueDescriptor{self->result()->handle};
	}

	HelError error() {
		return result()->error;
	}

	UniqueDescriptor descriptor() {
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

private:
	HelHandleResult *result() {
		return reinterpret_cast<HelHandleResult *>(OperationBase::element());
	}

	UniqueDescriptor _descriptor;
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
		return std::move(_descriptor);
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelHandleResult);
		if(!error())
			_descriptor = UniqueDescriptor{result()->handle};
	}

private:
	HelHandleResult *result() {
		return reinterpret_cast<HelHandleResult *>(OperationBase::element());
	}

	UniqueDescriptor _descriptor;
};

struct ImbueCredentials : Operation {
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

struct ExtractCredentials : Operation {
	HelError error() {
		return result()->error;
	}

	char *credentials() {
		return result()->credentials;
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelCredentialsResult);
	}

private:
	HelCredentialsResult *result() {
		return reinterpret_cast<HelCredentialsResult *>(OperationBase::element());
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
		return std::move(_descriptor);
	}

	void parse(void *&ptr) override {
		_element = ptr;
		ptr = (char *)ptr + sizeof(HelHandleResult);
		if(!error())
			_descriptor = UniqueDescriptor{result()->handle};
	}

private:
	HelHandleResult *result() {
		return reinterpret_cast<HelHandleResult *>(OperationBase::element());
	}

	UniqueDescriptor _descriptor;
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

struct AwaitEvent : Operation {
	HelError error() {
		return result()->error;
	}

	uint64_t sequence() { return result()->sequence; }
	uint32_t bitset() { return result()->bitset; }

private:
	HelEventResult *result() {
		return reinterpret_cast<HelEventResult *>(OperationBase::element());
	}
};

struct Observe : Operation {
	HelError error() {
		return result()->error;
	}

	int observation() {
		return result()->observation;
	}

	uint64_t sequence() {
		return result()->sequence;
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
	Submission(AwaitClock *operation,
			uint64_t counter, Dispatcher &dispatcher)
	: _result(operation) {
		uint64_t async_id;
		HEL_CHECK(helSubmitAwaitClock(counter, dispatcher.acquire(),
				reinterpret_cast<uintptr_t>(context()), &async_id));
		operation->setAsyncId(async_id);
	}

	Submission(BorrowedDescriptor space, ProtectMemory *operation,
			void *pointer, size_t length, uint32_t flags,
			Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitProtectMemory(space.getHandle(),
				pointer, length, flags,
				dispatcher.acquire(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(BorrowedDescriptor memory, ManageMemory *operation,
			Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitManageMemory(memory.getHandle(),
				dispatcher.acquire(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(BorrowedDescriptor memory, LockMemoryView *operation,
			uintptr_t offset, size_t size, Dispatcher &dispatcher)
	: _result(operation), _completeOperation{&LockMemoryView::completeOperation} {
		HEL_CHECK(helSubmitLockMemoryView(memory.getHandle(), offset, size,
				dispatcher.acquire(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(BorrowedDescriptor thread, Observe *operation,
			uint64_t in_seq, Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitObserve(thread.getHandle(), in_seq,
				dispatcher.acquire(),
				reinterpret_cast<uintptr_t>(context())));
	}

	Submission(BorrowedDescriptor descriptor, AwaitEvent *operation,
			uint64_t sequence, Dispatcher &dispatcher)
	: _result(operation) {
		HEL_CHECK(helSubmitAwaitEvent(descriptor.getHandle(), sequence,
				dispatcher.acquire(),
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

	void complete(ElementHandle element) override {
		_element = std::move(element);

		_result->_element = _element.data();
		if(_completeOperation)
			_completeOperation(_result);
		_pledge.set_value();
	}

	Operation *_result;
	void (*_completeOperation)(Operation *) = nullptr;
	async::promise<void> _pledge;
	ElementHandle _element;
};

template<typename R>
struct Item {
	Operation *operation;
	HelAction action;
};

inline Item<Offer> action(Offer *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionOffer;
	action.flags = flags;
	return {operation, action};
}

inline Item<Accept> action(Accept *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionAccept;
	action.flags = flags;
	return {operation, action};
}

inline Item<ImbueCredentials> action(ImbueCredentials *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionImbueCredentials;
	action.flags = flags;
	return {operation, action};
}

inline Item<ExtractCredentials> action(ExtractCredentials *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionExtractCredentials;
	action.flags = flags;
	return {operation, action};
}

inline Item<SendBuffer> action(SendBuffer *operation, const void *buffer, size_t length,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionSendFromBuffer;
	action.flags = flags;
	action.buffer = const_cast<void *>(buffer);
	action.length = length;
	return {operation, action};
}

inline Item<RecvInline> action(RecvInline *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionRecvInline;
	action.flags = flags;
	return {operation, action};
}

inline Item<RecvBuffer> action(RecvBuffer *operation, void *buffer, size_t length,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionRecvToBuffer;
	action.flags = flags;
	action.buffer = buffer;
	action.length = length;
	return {operation, action};
}

inline Item<PushDescriptor> action(PushDescriptor *operation, BorrowedDescriptor descriptor,
		uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionPushDescriptor;
	action.flags = flags;
	action.handle = descriptor.getHandle();
	return {operation, action};
}

inline Item<PullDescriptor> action(PullDescriptor *operation, uint32_t flags = 0) {
	HelAction action;
	action.type = kHelActionPullDescriptor;
	action.flags = flags;
	return {operation, action};
}

template<typename... I>
struct Transmission : private Context {
	Transmission(BorrowedDescriptor descriptor, std::array<HelAction, sizeof...(I)> actions,
			std::array<Operation *, sizeof...(I)> results, Dispatcher &dispatcher)
	: _results(results) {
		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitAsync(descriptor.getHandle(), actions.data(), sizeof...(I),
				dispatcher.acquire(),
				reinterpret_cast<uintptr_t>(context), 0));
	}

	Transmission(const Transmission &) = delete;

	Transmission &operator= (Transmission &other) = delete;

	async::result<void> async_wait() {
		return _pledge.async_get();
	}

private:
	void complete(ElementHandle element) override {
		_element = std::move(element);

		auto ptr = _element.data();
		for(size_t i = 0; i < sizeof...(I); ++i)
			_results[i]->parse(ptr);
		_pledge.set_value();
	}

	std::array<Operation *, sizeof...(I)> _results;
	async::promise<void> _pledge;
	ElementHandle _element;
};

inline Submission submitAwaitClock(AwaitClock *operation, uint64_t counter,
		Dispatcher &dispatcher) {
	return {operation, counter, dispatcher};
}

inline Submission submitProtectMemory(BorrowedDescriptor memory, ProtectMemory *operation,
		void *pointer, size_t length, uint32_t flags,
		Dispatcher &dispatcher) {
	return {memory, operation, pointer, length, flags, dispatcher};
}

inline Submission submitManageMemory(BorrowedDescriptor memory, ManageMemory *operation,
		Dispatcher &dispatcher) {
	return {memory, operation, dispatcher};
}

inline Submission submitLockMemoryView(BorrowedDescriptor memory, LockMemoryView *operation,
		uintptr_t offset, size_t size, Dispatcher &dispatcher) {
	return {memory, operation, offset, size, dispatcher};
}

inline Submission submitObserve(BorrowedDescriptor thread, Observe *operation,
		uint64_t in_seq, Dispatcher &dispatcher) {
	return {thread, operation, in_seq, dispatcher};
}

template<typename... I>
inline Transmission<I...> submitAsync(BorrowedDescriptor descriptor, Dispatcher &dispatcher,
		Item<I>... items) {
	std::array<HelAction, sizeof...(I)> actions{items.action...};
	std::array<Operation *, sizeof...(I)> results{items.operation...};
	return {descriptor, actions, results, dispatcher};
}

inline Submission submitAwaitEvent(BorrowedDescriptor descriptor, AwaitEvent *operation,
		uint64_t sequence, Dispatcher &dispatcher) {
	return {descriptor, operation, sequence, dispatcher};
}

} // namespace helix

namespace helix_ng {

using namespace helix;

// --------------------------------------------------------------------
// ExchangeMsgsSender
// --------------------------------------------------------------------

template <typename Results, typename Actions, typename Receiver>
struct ExchangeMsgsOperation : private Context {
	ExchangeMsgsOperation(BorrowedDescriptor lane, Actions actions, Receiver receiver)
	: lane_{std::move(lane)}, actions_{std::move(actions)}, receiver_{std::move(receiver)} { }

	void start() {
		auto helActions = frg::apply(chainActionArrays, actions_);

		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitAsync(lane_.getHandle(),
				helActions.data(), helActions.size(), Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context), 0));
	}

private:
	void complete(ElementHandle element) override {
		Results results;
		void *ptr = element.data();

		[&]<size_t ...p>(std::index_sequence<p...>) {
			(results.template get<p>().parse(ptr, element), ...);
		} (std::make_index_sequence<std::tuple_size<Results>::value>{});

		async::execution::set_value(receiver_, std::move(results));
	}

	BorrowedDescriptor lane_;
	Actions actions_;
	Receiver receiver_;
};

template <typename Results, typename Actions>
struct ExchangeMsgsSender {
	ExchangeMsgsSender(BorrowedDescriptor lane, Results, Actions actions)
	: lane_{std::move(lane)}, actions_{std::move(actions)} { }

	template<typename Receiver>
	ExchangeMsgsOperation<Results, Actions, Receiver> connect(Receiver receiver) {
		return {std::move(lane_), std::move(actions_), std::move(receiver)};
	}

private:
	BorrowedDescriptor lane_;
	Actions actions_;
};

template <typename Results, typename Actions>
async::sender_awaiter<ExchangeMsgsSender<Results, Actions>, Results>
operator co_await (ExchangeMsgsSender<Results, Actions> sender) {
	return {std::move(sender)};
}

template <typename ...Args>
auto exchangeMsgs(BorrowedDescriptor descriptor, Args &&...args) {
	return ExchangeMsgsSender{
		std::move(descriptor),
		createResultsTuple(args...),
		frg::tuple{std::forward<Args>(args)...}
	};
}

// --------------------------------------------------------------------
// Operations other than exchangeMsgs().
// --------------------------------------------------------------------

struct SynchronizeSpaceResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		error_ = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
};

struct [[nodiscard]] SynchronizeSpaceOperation : private Context {
	SynchronizeSpaceOperation(helix::BorrowedDescriptor spaceDescriptor,
			void *pointer, size_t size) {
		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitSynchronizeSpace(spaceDescriptor.getHandle(),
				pointer, size,
				Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context)));
	}

	SynchronizeSpaceOperation(const SynchronizeSpaceOperation &) = delete;

	SynchronizeSpaceOperation &operator= (const SynchronizeSpaceOperation &) = delete;

	auto operator co_await() {
		using async::operator co_await;
		return operator co_await(promise_.async_get());
	}

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		promise_.set_value(std::move(result));
	}

	async::promise<SynchronizeSpaceResult> promise_;
};

inline SynchronizeSpaceOperation synchronizeSpace(helix::BorrowedDescriptor spaceDescriptor,
		void *pointer, size_t size) {
	return {spaceDescriptor, pointer, size};
}

// --------------------------------------------------------------------
// Load/WriteMemory
// --------------------------------------------------------------------

struct [[nodiscard]] ReadMemoryOperation : private Context {
	ReadMemoryOperation(helix::BorrowedDescriptor descriptor,
			uintptr_t address, size_t length, void *buffer) {
		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitReadMemory(descriptor.getHandle(),
				address, length, buffer,
				Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context)));
	}

	ReadMemoryOperation(const ReadMemoryOperation &) = delete;

	ReadMemoryOperation &operator= (const ReadMemoryOperation &) = delete;

	auto operator co_await() {
		using async::operator co_await;
		return operator co_await(promise_.async_get());
	}

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		promise_.set_value(std::move(result));
	}

	async::promise<SynchronizeSpaceResult> promise_;
};

inline ReadMemoryOperation readMemory(helix::BorrowedDescriptor descriptor,
		uintptr_t address, size_t length, void *buffer) {
	return {descriptor, address, length, buffer};
}

struct [[nodiscard]] WriteMemoryOperation : private Context {
	WriteMemoryOperation(helix::BorrowedDescriptor descriptor,
			uintptr_t address, size_t length, const void *buffer) {
		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitWriteMemory(descriptor.getHandle(),
				address, length, buffer,
				Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context)));
	}

	WriteMemoryOperation(const WriteMemoryOperation &) = delete;

	WriteMemoryOperation &operator= (const WriteMemoryOperation &) = delete;

	auto operator co_await() {
		using async::operator co_await;
		return operator co_await(promise_.async_get());
	}

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		promise_.set_value(std::move(result));
	}

	async::promise<SynchronizeSpaceResult> promise_;
};

inline WriteMemoryOperation writeMemory(helix::BorrowedDescriptor descriptor,
		uintptr_t address, size_t length, const void *buffer) {
	return {descriptor, address, length, buffer};
}

} // namespace helix_ng

#endif // HELIX_HPP
