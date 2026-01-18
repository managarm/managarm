#pragma once

#include <async/mutex.hpp>
#include <frg/vector.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/cancel.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/mm-rc.hpp>

namespace thor {

struct IpcQueue;

// NOTE: The following structs mirror the Hel{Queue,Element} structs.
// They must be kept in sync!

static const int kUserNotifyCqProgress = (1 << 0);
static const int kUserNotifySupplySqChunks = (1 << 1);
static const int kUserNotifyAlert = (1 << 15);

static const int kKernelNotifySqProgress = (1 << 0);
static const int kKernelNotifySupplyCqChunks = (1 << 1);

struct QueueStruct {
	int userNotify;
	int kernelNotify;
	int cqFirst;
	int sqFirst;
};

static const int kNextPresent = (1 << 24);

static const int kProgressMask = 0xFFFFFF;
static const int kProgressDone = (1 << 25);

struct ChunkStruct {
	int next;
	int progressFutex;
	char buffer[];
};

struct IpcQueue;
struct ImmediateMemory;

// Called from IpcQueue::processSq() to handle SQ elements.
// Implemented in hel.cpp.
void submitFromSq(smarter::shared_ptr<IpcQueue> queue, uint32_t opcode,
		ImmediateMemory *memory, size_t dataOffset, size_t length, uintptr_t context);

struct ElementStruct {
	unsigned int length;
	unsigned int opcode;
	void *context;
};

struct QueueSource {
	void setup(void *pointer_, size_t size_) {
		pointer = pointer_;
		size = size_;
	}

	void *pointer;
	size_t size;
	const QueueSource *link;
};

struct IpcQueue : CancelRegistry {
private:
	using Address = uintptr_t;

public:
	IpcQueue(unsigned int numChunks, size_t chunkSize, unsigned int numSqChunks);

	IpcQueue(const IpcQueue &) = delete;

	IpcQueue &operator= (const IpcQueue &) = delete;

	// Contract: must be called after construction.
	smarter::borrowed_ptr<IpcQueue> selfPtr;

	smarter::shared_ptr<MemoryView> getMemory() {
		return _memory;
	}

	bool validSize(size_t size);

	void setupChunk(size_t index, smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer);

	coroutine<void> submit(QueueSource *source, uintptr_t context);

	// Processes pending SQ elements. Called from helDriveQueue().
	void processSq();

	void raiseCqEvent() {
		_cqEvent.raise();
	}

	bool checkUserNotify() {
		auto head = _memory->accessImmediate<QueueStruct>(0);
		auto userNotify = __atomic_load_n(&head->userNotify, __ATOMIC_ACQUIRE);
		return userNotify & (kUserNotifyCqProgress | kUserNotifyAlert);
	}

	auto waitUserEvent(async::cancellation_token ct) {
		return _userEvent.async_wait_if([this] () -> bool {
			auto head = _memory->accessImmediate<QueueStruct>(0);
			auto userNotify = __atomic_load_n(&head->userNotify, __ATOMIC_ACQUIRE);
			return !(userNotify & (kUserNotifyCqProgress | kUserNotifyAlert));
		}, ct);
	}

	void alert() {
		auto head = _memory->accessImmediate<QueueStruct>(0);
		auto userNotify = __atomic_fetch_or(&head->userNotify,
				kUserNotifyAlert, __ATOMIC_RELEASE);
		if(!(userNotify & kUserNotifyAlert)) {
			_userEvent.raise();
		}
	}

private:
	smarter::shared_ptr<ImmediateMemory> _memory;

	size_t _chunkSize;

	frg::vector<size_t, KernelAlloc> _chunkOffsets;

	// CQ state.
	async::mutex _cqMutex;

	// True if _currentChunk and _currentProgress are valid.
	bool _haveCqChunk{false};
	// Chunk that we are currently processing.
	int _currentChunk;
	// Progress into the current chunk.
	int _currentProgress;

	// Event raised when userspace supplies new CQ chunks.
	async::recurring_event _cqEvent;
	// Event raised when kernel makes progress (i.e., userNotify changes).
	async::recurring_event _userEvent;

	// SQ state.
	async::mutex _sqMutex;

	unsigned int _numCqChunks{0};
	unsigned int _numSqChunks{0};
	int _sqCurrentChunk{0};
	int _sqCurrentProgress{0};
	int _sqTailChunk{0};
};

} // namespace thor
