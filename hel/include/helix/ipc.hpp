#pragma once

#include <assert.h>
#include <tuple>
#include <array>
#include <vector>
#include <span>

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
	static Dispatcher &global();

	Dispatcher()
	: _handle{kHelNullHandle}, _queue{nullptr},
			_numCqChunks{0}, _numSqChunks{0}, _chunkSize{0},
			_retrieveChunk{0}, _tailChunk{0}, _lastProgress{0},
			_sqCurrentChunk{0}, _sqProgress{0} { }

	Dispatcher(const Dispatcher &) = delete;

	Dispatcher &operator= (const Dispatcher &) = delete;

	HelHandle acquire() {
		if(!_handle) {
			_numCqChunks = 8;
			_numSqChunks = 8;
			_chunkSize = 4096;

			HelQueueParameters params {
				.flags = 0,
				.numChunks = _numCqChunks,
				.chunkSize = _chunkSize,
				.numSqChunks = _numSqChunks,
			};
			HEL_CHECK(helCreateQueue(&params, &_handle));
			_nextAsyncId = 1;

			auto totalChunks = _numCqChunks + _numSqChunks;
			auto chunksOffset = (sizeof(HelQueue) + 63) & ~size_t(63);
			auto reservedPerChunk = (sizeof(HelChunk) + _chunkSize + 63) & ~size_t(63);
			auto overallSize = chunksOffset + totalChunks * reservedPerChunk;

			void *mapping;
			HEL_CHECK(helMapMemory(_handle, kHelNullHandle, nullptr,
					0, (overallSize + 0xFFF) & ~size_t(0xFFF),
					kHelMapProtRead | kHelMapProtWrite, &mapping));

			_queue = reinterpret_cast<HelQueue *>(mapping);
			auto chunksPtr = reinterpret_cast<std::byte *>(mapping) + chunksOffset;
			for(unsigned int i = 0; i < totalChunks; ++i)
				_chunks[i] = reinterpret_cast<HelChunk *>(chunksPtr + i * reservedPerChunk);

			// Reset all CQ chunks.
			for (unsigned int i = 0; i < _numCqChunks; ++i)
				_resetChunk(i);

			// Set up CQ: chunks 0 to numCqChunks-1.
			__atomic_store_n(&_queue->cqFirst, 0 | kHelNextPresent, __ATOMIC_RELEASE);

			// Supply the remaining CQ chunks.
			_tailChunk = 0;
			for (unsigned int i = 1; i < _numCqChunks; ++i)
				_supplyChunk(i);
			_retrieveChunk = 0;

			// SQ is initialized by the kernel. Read sqFirst to get the first SQ chunk.
			if (_numSqChunks > 0) {
				auto sqFirst = __atomic_load_n(&_queue->sqFirst, __ATOMIC_ACQUIRE);
				_sqCurrentChunk = sqFirst & ~kHelNextPresent;
				_sqProgress = 0;
			}

			_wakeHeadFutex();
		}

		return _handle;
	}

	uint64_t makeAsyncId() {
		return _nextAsyncId++;
	}

	void wait() {
		while(true) {
			bool done;
			_waitProgressFutex(&done);
			if(done) {
				auto cn = _retrieveChunk;
				auto next = __atomic_load_n(&_chunks[cn]->next, __ATOMIC_ACQUIRE);
				_surrender(cn);

				_lastProgress = 0;
				_retrieveChunk = next & ~kHelNextPresent;
				continue;
			}

			// Dequeue the next element.
			auto ptr = (char *)_chunks[_retrieveChunk] + sizeof(HelChunk) + _lastProgress;
			auto element = reinterpret_cast<HelElement *>(ptr);
			_lastProgress += sizeof(HelElement) + element->length;

			auto context = reinterpret_cast<Context *>(element->context);
			_refCounts[_retrieveChunk]++;
			context->complete(ElementHandle{this, _retrieveChunk,
					ptr + sizeof(HelElement)});
			return;
		}
	}

private:
	void _surrender(int cn) {
		assert(_refCounts[cn] > 0);
		if(_refCounts[cn]-- > 1)
			return;
		_resetChunk(cn);
		_supplyChunk(cn);
	}

	void _resetChunk(int cn) {
		// Reset the chunk's state.
		_chunks[cn]->next = 0;
		_chunks[cn]->progressFutex = 0;

		// Internal bookkeeping.
		_refCounts[cn] = 1;
	}

	void _supplyChunk(int cn) {
		__atomic_store_n(&_chunks[_tailChunk]->next, cn | kHelNextPresent, __ATOMIC_RELEASE);
		_tailChunk = cn;
		_wakeHeadFutex();
	}

	void _reference(int cn) {
		_refCounts[cn]++;
	}

public:
	// Push an element to the SQ using a gather list.
	void pushSq(uint32_t opcode, uintptr_t context,
			std::span<const std::span<const std::byte>> segments) {
		acquire();

		size_t dataLength = 0;
		for (auto seg : segments)
			dataLength += seg.size();

		auto elementSize = sizeof(HelElement) + dataLength;

		// Check if we need to move to the next chunk.
		if (_sqProgress + elementSize > _chunkSize) {
			// Wait for next chunk to become available.
			auto nextWord = __atomic_load_n(&_chunks[_sqCurrentChunk]->next, __ATOMIC_ACQUIRE);
			while(!(nextWord & kHelNextPresent)) {
				__atomic_fetch_and(&_queue->userNotify, ~kHelUserNotifySupplySqChunks, __ATOMIC_ACQUIRE);

				nextWord = __atomic_load_n(&_chunks[_sqCurrentChunk]->next, __ATOMIC_ACQUIRE);
				if(nextWord & kHelNextPresent)
					break;

				HEL_CHECK(helDriveQueue(_handle, 0));
			}

			// Mark current chunk as done.
			__atomic_store_n(&_chunks[_sqCurrentChunk]->progressFutex,
					_sqProgress | kHelProgressDone, __ATOMIC_RELEASE);

			// Signal the kernel.
			// Note: We do not call helDriveQueue() here; instead this is done at the next wait().
			__atomic_fetch_or(&_queue->kernelNotify, kHelKernelNotifySqProgress, __ATOMIC_RELEASE);

			_sqCurrentChunk = nextWord & ~kHelNextPresent;
			_sqProgress = 0;
		}

		auto ptr = reinterpret_cast<char *>(_chunks[_sqCurrentChunk]) + sizeof(HelChunk) + _sqProgress;
		auto element = reinterpret_cast<HelElement *>(ptr);
		element->length = dataLength;
		element->opcode = opcode;
		element->context = reinterpret_cast<void *>(context);

		// Copy each segment.
		size_t offset = 0;
		for (auto seg : segments) {
			memcpy(ptr + sizeof(HelElement) + offset, seg.data(), seg.size());
			offset += seg.size();
		}

		_sqProgress += elementSize;

		// Signal the kernel that new SQ elements are available.
		// Note: We do not call helDriveQueue() here; instead this is done at the next wait().
		__atomic_store_n(&_chunks[_sqCurrentChunk]->progressFutex, _sqProgress, __ATOMIC_RELEASE);
		__atomic_fetch_or(&_queue->kernelNotify, kHelKernelNotifySqProgress, __ATOMIC_RELEASE);
	}

	inline void cancel(uint64_t cancellationTag) {
		HelSqCancel sqData{};
		sqData.cancellationTag = cancellationTag;
		std::array segments{std::as_bytes(std::span{&sqData, 1})};
		pushSq(kHelSubmitCancel, 0, segments);
	}

private:
	void _wakeHeadFutex() {
		auto futex = __atomic_fetch_or(&_queue->kernelNotify, kHelKernelNotifySupplyCqChunks, __ATOMIC_RELEASE);
		if(!(futex & kHelKernelNotifySupplyCqChunks))
			HEL_CHECK(helDriveQueue(_handle, 0));
	}

	void _waitProgressFutex(bool *done) {
		auto check = [&] () -> bool {
			auto progress = __atomic_load_n(&_chunks[_retrieveChunk]->progressFutex, __ATOMIC_ACQUIRE);
			assert(!(progress & ~(kHelProgressMask | kHelProgressDone)));
			if(_lastProgress != (progress & kHelProgressMask)) {
				*done = false;
				return true;
			}else if(progress & kHelProgressDone) {
				*done = true;
				return true;
			}
			return false;
		};

		if (check())
			return;
		while(true) {
			__atomic_fetch_and(&_queue->userNotify, ~kHelUserNotifyCqProgress, __ATOMIC_ACQUIRE);
			if (check())
				return;
			auto e = helDriveQueue(_handle, kHelDriveWaitCqProgress);
			if (e == kHelErrCancelled)
				continue;
			HEL_CHECK(e);
		}
	}

private:
	HelHandle _handle;
	HelQueue *_queue;
	HelChunk *_chunks[16];

	// Queue parameters.
	unsigned int _numCqChunks;
	unsigned int _numSqChunks;
	size_t _chunkSize;

	uint64_t _nextAsyncId{0};

	// CQ state.
	// Chunk that we are currently retrieving from.
	int _retrieveChunk;
	// Tail of the CQ chunk list (where we append new chunks).
	int _tailChunk;
	// Progress into the current CQ chunk.
	int _lastProgress;
	// Per-chunk reference counts.
	int _refCounts[16];

	// SQ state.
	// Chunk that we are currently writing to.
	int _sqCurrentChunk;
	// Progress into the current SQ chunk.
	int _sqProgress;
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
		auto asyncId = dispatcher.makeAsyncId();

		HelSqAwaitClock sqData;
		sqData.counter = counter;
		sqData.cancellationTag = asyncId;
		std::array segments{std::as_bytes(std::span{&sqData, 1})};
		dispatcher.pushSq(kHelSubmitAwaitClock,
				reinterpret_cast<uintptr_t>(context()), segments);

		operation->setAsyncId(asyncId);
	}

	Submission(BorrowedDescriptor space, ProtectMemory *operation,
			void *pointer, size_t length, uint32_t flags,
			Dispatcher &dispatcher)
	: _result(operation) {
		HelSqProtectMemory header;
		header.spaceHandle = space.getHandle();
		header.pointer = pointer;
		header.size = length;
		header.flags = flags;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitProtectMemory,
				reinterpret_cast<uintptr_t>(context()), segments);
	}

	Submission(BorrowedDescriptor memory, ManageMemory *operation,
			Dispatcher &dispatcher)
	: _result(operation) {
		HelSqManageMemory header;
		header.handle = memory.getHandle();

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitManageMemory,
				reinterpret_cast<uintptr_t>(context()), segments);
	}

	Submission(BorrowedDescriptor memory, LockMemoryView *operation,
			uintptr_t offset, size_t size, Dispatcher &dispatcher)
	: _result(operation) {
		HelSqLockMemoryView header;
		header.handle = memory.getHandle();
		header.offset = offset;
		header.size = size;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitLockMemoryView,
				reinterpret_cast<uintptr_t>(context()), segments);
	}

	Submission(BorrowedDescriptor thread, Observe *operation,
			uint64_t in_seq, Dispatcher &dispatcher)
	: _result(operation) {
		HelSqObserve header;
		header.handle = thread.getHandle();
		header.sequence = in_seq;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		dispatcher.pushSq(kHelSubmitObserve,
				reinterpret_cast<uintptr_t>(context()), segments);
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

		HelSqExchangeMsgs header;
		header.lane = lane_.getHandle();
		header.count = helActions.size();
		header.flags = 0;

		std::array segments{
			std::as_bytes(std::span{&header, 1}),
			std::as_bytes(std::span{helActions.data(), helActions.size()})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitExchangeMsgs,
				reinterpret_cast<uintptr_t>(context), segments);
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

		std::span<const std::span<const std::byte>> segments;
		Dispatcher::global().pushSq(kHelSubmitAsyncNop,
				reinterpret_cast<uintptr_t>(context), segments);
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
		HelSqSynchronizeSpace header;
		header.spaceHandle = space_.getHandle();
		header.pointer = pointer_;
		header.size = size_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitSynchronizeSpace,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	SynchronizeSpaceOperation(const SynchronizeSpaceOperation &) = delete;
	SynchronizeSpaceOperation &operator= (const SynchronizeSpaceOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
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
		HelSqReadMemory header;
		header.handle = descriptor_.getHandle();
		header.address = address_;
		header.length = length_;
		header.buffer = buffer_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitReadMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	ReadMemoryOperation(const ReadMemoryOperation &) = delete;
	ReadMemoryOperation &operator= (const ReadMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
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
		HelSqWriteMemory header;
		header.handle = descriptor_.getHandle();
		header.address = address_;
		header.length = length_;
		header.buffer = buffer_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitWriteMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	WriteMemoryOperation(const WriteMemoryOperation &) = delete;
	WriteMemoryOperation &operator= (const WriteMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		SynchronizeSpaceResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
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
		asyncId_ = Dispatcher::global().makeAsyncId();

		HelSqAwaitEvent header;
		header.handle = event_.getHandle();
		header.sequence = sequence_;
		header.cancellationTag = asyncId_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitAwaitEvent,
				reinterpret_cast<uintptr_t>(context), segments);

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
		Dispatcher::global().cancel(asyncId_);
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

// --------------------------------------------------------------------
// ResizeMemory
// --------------------------------------------------------------------

struct ResizeMemoryResult {
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
struct ResizeMemoryOperation : private Context {
	ResizeMemoryOperation(BorrowedDescriptor memory, size_t newSize, Receiver r)
	: memory_{std::move(memory)}, newSize_{newSize}, r_{std::move(r)} {}

	void start() {
		HelSqResizeMemory header;
		header.handle = memory_.getHandle();
		header.newSize = newSize_;

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitResizeMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	ResizeMemoryOperation(const ResizeMemoryOperation &) = delete;
	ResizeMemoryOperation &operator= (const ResizeMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		ResizeMemoryResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor memory_;
	size_t newSize_;
	Receiver r_;
};

struct [[nodiscard]] ResizeMemorySender {
	using value_type = ResizeMemoryResult;

	ResizeMemorySender(BorrowedDescriptor memory, size_t newSize)
	: memory_{std::move(memory)}, newSize_{newSize} { }

	template<typename Receiver>
	ResizeMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(memory_), newSize_, std::move(receiver)};
	}

private:
	BorrowedDescriptor memory_;
	size_t newSize_;
};

inline async::sender_awaiter<ResizeMemorySender, ResizeMemoryResult>
operator co_await (ResizeMemorySender sender) {
	return {std::move(sender)};
}

inline auto resizeMemory(BorrowedDescriptor memory, size_t newSize) {
	return ResizeMemorySender{std::move(memory), newSize};
}

// --------------------------------------------------------------------
// ForkMemory
// --------------------------------------------------------------------

struct ForkMemoryResult {
	HelError error() {
		assert(valid_);
		return error_;
	}

	UniqueDescriptor descriptor() {
		assert(valid_);
		HEL_CHECK(error());
		return std::move(descriptor_);
	}

	void parse(void *&ptr, const ElementHandle &) {
		auto result = reinterpret_cast<HelHandleResult *>(ptr);
		error_ = result->error;
		if(!error_)
			descriptor_ = UniqueDescriptor{result->handle};
		ptr = (char *)ptr + sizeof(HelHandleResult);
		valid_ = true;
	}

private:
	bool valid_ = false;
	HelError error_;
	UniqueDescriptor descriptor_;
};

template <typename Receiver>
struct ForkMemoryOperation : private Context {
	ForkMemoryOperation(BorrowedDescriptor memory, Receiver r)
	: memory_{std::move(memory)}, r_{std::move(r)} {}

	void start() {
		HelSqForkMemory header;
		header.handle = memory_.getHandle();

		std::array segments{
			std::as_bytes(std::span{&header, 1})
		};

		auto context = static_cast<Context *>(this);
		Dispatcher::global().pushSq(kHelSubmitForkMemory,
				reinterpret_cast<uintptr_t>(context), segments);
	}

	ForkMemoryOperation(const ForkMemoryOperation &) = delete;
	ForkMemoryOperation &operator= (const ForkMemoryOperation &) = delete;

private:
	void complete(ElementHandle element) override {
		ForkMemoryResult result;
		void *ptr = element.data();
		result.parse(ptr, element);
		async::execution::set_value(r_, std::move(result));
	}

	BorrowedDescriptor memory_;
	Receiver r_;
};

struct [[nodiscard]] ForkMemorySender {
	using value_type = ForkMemoryResult;

	ForkMemorySender(BorrowedDescriptor memory)
	: memory_{std::move(memory)} { }

	template<typename Receiver>
	ForkMemoryOperation<Receiver> connect(Receiver receiver) {
		return {std::move(memory_), std::move(receiver)};
	}

private:
	BorrowedDescriptor memory_;
};

inline async::sender_awaiter<ForkMemorySender, ForkMemoryResult>
operator co_await (ForkMemorySender sender) {
	return {std::move(sender)};
}

inline auto forkMemory(BorrowedDescriptor memory) {
	return ForkMemorySender{std::move(memory)};
}

} // namespace helix_ng
