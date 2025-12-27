#pragma once

#include <assert.h>
#include <tuple>
#include <array>

#include <async/oneshot-event.hpp>

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
		HEL_CHECK(helTransferDescriptor(getHandle(), kHelThisUniverse, kHelTransferDescriptorOut, &new_handle));
		return UniqueResource<Tag>(new_handle);
	}
};

struct Lane { };
using UniqueLane = UniqueResource<Lane>;
using BorrowedLane = BorrowedResource<Lane>;

inline std::pair<UniqueLane, UniqueLane> createStream(bool attach_credentials = false) {
	HelHandle first_handle, second_handle;
	HEL_CHECK(helCreateStream(&first_handle, &second_handle, attach_credentials));
	return { UniqueLane(first_handle), UniqueLane(second_handle) };
}

struct Irq { };
using UniqueIrq = UniqueResource<Irq>;
using BorrowedIrq = BorrowedResource<Irq>;

struct OperationBase {
	friend struct Dispatcher;

	OperationBase()
	: _asyncId(0) { }

	virtual ~OperationBase() { }

protected:
	int64_t _asyncId;
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

	virtual void parse(const void *) = 0;

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
			HelQueueParameters params {
				.flags = 0,
				.ringShift = sizeShift,
				.numChunks = 16,
				.chunkSize = 4096,
			};
			HEL_CHECK(helCreateQueue(&params, &_handle));

			auto chunksOffset = (sizeof(HelQueue) + (sizeof(int) << sizeShift) + 63) & ~size_t(63);
			auto reservedPerChunk = (sizeof(HelChunk) + params.chunkSize + 63) & ~size_t(63);
			auto overallSize = chunksOffset + params.numChunks * reservedPerChunk;

			void *mapping;
			HEL_CHECK(helMapMemory(_handle, kHelNullHandle, nullptr,
					0, (overallSize + 0xFFF) & ~size_t(0xFFF),
					kHelMapProtRead | kHelMapProtWrite, &mapping));

			_queue = reinterpret_cast<HelQueue *>(mapping);
			auto chunksPtr = reinterpret_cast<std::byte *>(mapping) + chunksOffset;
			for(unsigned int i = 0; i < 16; ++i)
				_chunks[i] = reinterpret_cast<HelChunk *>(chunksPtr + i * reservedPerChunk);
		}

		return _handle;
	}

	void wait() {
		while(true) {
			// TODO: Initialize all chunks when setting up the queue.
			if(_retrieveIndex == _nextIndex) {
				assert(_activeChunks < 16);

				// Reset and enqueue the new chunk.
				_chunks[_activeChunks]->progressFutex = 0;

				_queue->indexQueue[_nextIndex & ((1 << sizeShift) - 1)] = _activeChunks;
				_nextIndex = ((_nextIndex + 1) & kHelHeadMask);
				_wakeHeadFutex();

				_refCounts[_activeChunks] = 1;
				_activeChunks++;
				continue;
			}else if (_hadWaiters && _activeChunks < (1 << sizeShift)) {
				assert(_activeChunks < 16);

				// Reset and enqueue the new chunk.
				_chunks[_activeChunks]->progressFutex = 0;

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
			HEL_CHECK(helFutexWake(&_queue->headFutex, UINT32_MAX));
			_hadWaiters = true;
		}
	}

	void _waitProgressFutex(bool *done) {
		while(true) {
			auto futex = __atomic_load_n(&_retrieveChunk()->progressFutex, __ATOMIC_ACQUIRE);
			assert(!(futex & ~(kHelProgressMask | kHelProgressWaiters | kHelProgressDone)));
			do {
				if(_lastProgress != (futex & kHelProgressMask)) {
					*done = false;
					return;
				}else if(futex & kHelProgressDone) {
					*done = true;
					return;
				}

				if(futex & kHelProgressWaiters)
					break; // Waiters bit is already set (in a previous iteration).
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
	HelChunk *_chunks[16];

	int _activeChunks;
	bool _hadWaiters;

	// Index of the chunk that we are currently retrieving/inserting next.
	int _retrieveIndex;
	int _nextIndex;

	// Progress into the current chunk.
	int _lastProgress;

	// Per-chunk reference counts.
	int _refCounts[16];
};

inline void CurrentDispatcherToken::wait() {
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
		return result_.error;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelSimpleResult result_;
};

struct ProtectMemory : Operation {
	HelError error() {
		return result_.error;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelSimpleResult result_;
};

struct ManageMemory : Operation {
	HelError error() {
		return result_.error;
	}

	int type() {
		return result_.type;
	}

	uintptr_t offset() {
		return result_.offset;
	}

	size_t length() {
		return result_.length;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelManageResult result_;
};

struct LockMemoryView : Operation {
	HelError error() {
		return result_.error;
	}

	UniqueDescriptor descriptor() {
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));

		if(!error())
			_descriptor = UniqueDescriptor{result_.handle};
	}

	HelHandleResult result_;

	UniqueDescriptor _descriptor;
};

struct Observe : Operation {
	HelError error() {
		return result_.error;
	}

	unsigned int observation() {
		return result_.observation;
	}

	uint64_t sequence() {
		return result_.sequence;
	}

private:
	void parse(const void *ptr) override {
		memcpy(&result_, ptr, sizeof(result_));
	}

	HelObserveResult result_;
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
	: _result(operation) {
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

	Submission(const Submission &) = delete;

	Submission &operator= (Submission &other) = delete;

	auto async_wait() {
		return _ev.wait();
	}

private:
	Context *context() {
		return this;
	}

	void complete(ElementHandle element) override {
		auto ptr = element.data();
		_result->parse(ptr);
		_ev.raise();
	}

	Operation *_result;
	async::oneshot_primitive _ev;
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
struct [[nodiscard]] ExchangeMsgsSender {
	using value_type = Results;

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

template <typename Receiver>
struct AsyncNopOperation : private Context {
	AsyncNopOperation(Receiver receiver)
	: receiver_{std::move(receiver)} { }

	void start() {
		auto context = static_cast<Context *>(this);

		HEL_CHECK(helSubmitAsyncNop(Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context)));
	}

private:
	void complete(ElementHandle element) override {
		AsyncNopResult result;
		void *ptr = element.data();

		result.parse(ptr, element);

		async::execution::set_value(receiver_, std::move(result));
	}

	Receiver receiver_;
};

struct [[nodiscard]] AsyncNopSender {
	using value_type = AsyncNopResult;

	AsyncNopSender() = default;

	template<typename Receiver>
	AsyncNopOperation<Receiver> connect(Receiver receiver) {
		return {std::move(receiver)};
	}
};

inline async::sender_awaiter<AsyncNopSender, AsyncNopResult>
operator co_await (AsyncNopSender sender) {
	return {std::move(sender)};
}

inline auto asyncNop() {
	return AsyncNopSender{};
}

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

template <typename Receiver>
struct SynchronizeSpaceOperation : private Context {
	SynchronizeSpaceOperation(BorrowedDescriptor space,
			void *pointer, size_t size, Receiver r)
	: space_{std::move(space)}, pointer_{pointer}, size_{size}, r_{std::move(r)} {}

	void start() {
		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitSynchronizeSpace(space_.getHandle(),
				pointer_, size_,
				Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context)));
	}

	SynchronizeSpaceOperation(const SynchronizeSpaceOperation &) = delete;
	SynchronizeSpaceOperation &operator= (const SynchronizeSpaceOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value_noinline(r_, std::move(result));
	}

	BorrowedDescriptor space_;
	void *pointer_;
	size_t size_;
	Receiver r_;
};

struct [[nodiscard]] SynchronizeSpaceSender {
	using value_type = SynchronizeSpaceResult;

	SynchronizeSpaceSender(BorrowedDescriptor space, void *pointer, size_t size)
	: space_{std::move(space)}, pointer_{pointer}, size_{size} { }

	template<typename Receiver>
	SynchronizeSpaceOperation<Receiver> connect(Receiver receiver) {
		return {std::move(space_), pointer_, size_, std::move(receiver)};
	}

private:
	BorrowedDescriptor space_;
	void *pointer_;
	size_t size_;
};

inline async::sender_awaiter<SynchronizeSpaceSender, SynchronizeSpaceResult>
operator co_await (SynchronizeSpaceSender sender) {
	return {std::move(sender)};
}

inline auto synchronizeSpace(BorrowedDescriptor space, void *pointer, size_t size) {
	return SynchronizeSpaceSender{std::move(space), pointer, size};
}

// --------------------------------------------------------------------
// Read/WriteMemory
// --------------------------------------------------------------------

template <typename Receiver>
struct ReadMemoryOperation : private Context {
	ReadMemoryOperation(BorrowedDescriptor descriptor,
			uintptr_t address, size_t length, void *buffer, Receiver r)
	: descriptor_{std::move(descriptor)}, address_{address}, length_{length},
		buffer_{buffer}, r_{std::move(r)} {}

	void start() {
		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitReadMemory(descriptor_.getHandle(),
				address_, length_, buffer_,
				Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context)));
	}

	ReadMemoryOperation(const ReadMemoryOperation &) = delete;
	ReadMemoryOperation &operator= (const ReadMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value_noinline(r_, std::move(result));
	}

	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	void *buffer_;
	Receiver r_;
};

struct [[nodiscard]] ReadMemorySender {
	using value_type = SynchronizeSpaceResult;

	ReadMemorySender(BorrowedDescriptor descriptor, uintptr_t address,
			size_t length, void *buffer)
	: descriptor_{std::move(descriptor)}, address_{address},
		length_{length}, buffer_{buffer} { }

	template<typename Receiver>
	ReadMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(descriptor_), address_, length_, buffer_, std::move(receiver)};
	}

private:
	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	void *buffer_;
};

inline async::sender_awaiter<ReadMemorySender, SynchronizeSpaceResult>
operator co_await (ReadMemorySender sender) {
	return {std::move(sender)};
}

inline auto readMemory(BorrowedDescriptor descriptor,
		uintptr_t address, size_t length, void *buffer) {
	return ReadMemorySender{descriptor, address, length, buffer};
}

template <typename Receiver>
struct WriteMemoryOperation : private Context {
	WriteMemoryOperation(BorrowedDescriptor descriptor,
			uintptr_t address, size_t length, const void *buffer, Receiver r)
	: descriptor_{std::move(descriptor)}, address_{address}, length_{length},
		buffer_{buffer}, r_{std::move(r)} { }

	void start() {
		auto context = static_cast<Context *>(this);
		HEL_CHECK(helSubmitWriteMemory(descriptor_.getHandle(),
				address_, length_, buffer_,
				Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context)));
	}

	WriteMemoryOperation(const WriteMemoryOperation &) = delete;
	WriteMemoryOperation &operator= (const WriteMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value_noinline(r_, std::move(result));
	}

	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	const void *buffer_;
	Receiver r_;
};

struct [[nodiscard]] WriteMemorySender {
	using value_type = SynchronizeSpaceResult;

	WriteMemorySender(BorrowedDescriptor descriptor, uintptr_t address,
			size_t length, const void *buffer)
	: descriptor_{std::move(descriptor)}, address_{address},
		length_{length}, buffer_{buffer} { }

	template<typename Receiver>
	WriteMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(descriptor_), address_, length_, buffer_, std::move(receiver)};
	}

private:
	BorrowedDescriptor descriptor_;
	uintptr_t address_;
	size_t length_;
	const void *buffer_;
};

inline async::sender_awaiter<WriteMemorySender, SynchronizeSpaceResult>
operator co_await (WriteMemorySender sender) {
	return {std::move(sender)};
}

inline auto writeMemory(BorrowedDescriptor descriptor,
		uintptr_t address, size_t length, const void *buffer) {
	return WriteMemorySender{descriptor, address, length, buffer};
}

// --------------------------------------------------------------------
// AwaitEvent
// --------------------------------------------------------------------

template <typename Receiver>
struct AwaitEventOperation : private Context {
	AwaitEventOperation(BorrowedDescriptor event, uint64_t sequence, async::cancellation_token ct, Receiver receiver)
	: event_{std::move(event)}, sequence_{sequence}, ct_{ct}, receiver_{std::move(receiver)} { }

	void start() {
		auto context = static_cast<Context *>(this);

		HEL_CHECK(helSubmitAwaitEvent(event_.getHandle(), sequence_,
				Dispatcher::global().acquire(),
				reinterpret_cast<uintptr_t>(context), &asyncId_));

		cb_.emplace(ct_, this);
	}

private:
	void complete(ElementHandle element) override {
		cb_ = std::nullopt;

		AwaitEventResult result;
		void *ptr = element.data();

		result.parse(ptr, element);

		async::execution::set_value(receiver_, std::move(result));
	}

	void cancel() {
		HEL_CHECK(helCancelAsync(Dispatcher::global().acquire(), asyncId_));
	}

	BorrowedDescriptor event_;
	uint64_t sequence_;
	async::cancellation_token ct_;
	std::optional<async::cancellation_callback<frg::bound_mem_fn<&AwaitEventOperation::cancel>>> cb_ = std::nullopt;
	uint64_t asyncId_;
	Receiver receiver_;
};

struct [[nodiscard]] AwaitEventSender {
	using value_type = AwaitEventResult;

	AwaitEventSender(BorrowedDescriptor event, uint64_t sequence, async::cancellation_token ct)
	: event_{std::move(event)}, sequence_{sequence}, ct_{ct} { }

	template<typename Receiver>
	AwaitEventOperation<Receiver> connect(Receiver receiver) {
		return {std::move(event_), sequence_, ct_, std::move(receiver)};
	}

private:
	BorrowedDescriptor event_;
	uint64_t sequence_;
	async::cancellation_token ct_;
};

inline async::sender_awaiter<AwaitEventSender, AwaitEventResult>
operator co_await (AwaitEventSender sender) {
	return {std::move(sender)};
}

inline auto awaitEvent(BorrowedDescriptor event, uint64_t sequence, async::cancellation_token ct = {}) {
	return AwaitEventSender{
		std::move(event),
		sequence,
		ct
	};
}

} // namespace helix_ng
