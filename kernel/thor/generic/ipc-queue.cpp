
#include <string.h>

#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipc-queue.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

// ----------------------------------------------------------------------------
// IpcQueue
// ----------------------------------------------------------------------------

IpcQueue::IpcQueue(unsigned int numChunks, size_t chunkSize, unsigned int numSqChunks)
: _chunkSize{chunkSize}, _chunkOffsets{*kernelAlloc},
		_currentChunk{0}, _currentProgress{0},
		_numCqChunks{numChunks}, _numSqChunks{numSqChunks} {
	auto totalChunks = numChunks + numSqChunks;
	auto chunksOffset = (sizeof(QueueStruct) + 63) & ~size_t(63);
	auto reservedPerChunk = (sizeof(ChunkStruct) + chunkSize + 63) & ~size_t(63);
	auto overallSize = chunksOffset + totalChunks * reservedPerChunk;

	// Setup internal state.
	_memory = smarter::allocate_shared<ImmediateMemory>(*kernelAlloc, overallSize);
	_memory->selfPtr = _memory;
	_mapping = ImmediateWindow{_memory};
	_chunkOffsets.resize(totalChunks);
	for(unsigned int i = 0; i < totalChunks; ++i)
		_chunkOffsets[i] = chunksOffset + i * reservedPerChunk;

	// Initialize SQ chunks if configured.
	if(numSqChunks > 0) {
		auto head = _mapping.access<QueueStruct>(0);

		// Reset all SQ chunks.
		for(unsigned int i = numChunks; i < totalChunks; ++i) {
			auto chunkOffset = _chunkOffsets[i];
			auto chunkHead = _mapping.access<ChunkStruct>(chunkOffset);
			chunkHead->next = 0;
			chunkHead->progressFutex = 0;
		}

		// Link SQ chunks together.
		for(unsigned int i = numChunks; i < totalChunks - 1; ++i) {
			auto chunkOffset = _chunkOffsets[i];
			auto chunkHead = _mapping.access<ChunkStruct>(chunkOffset);
			chunkHead->next = (i + 1) | kNextPresent;
		}

		// Set sqFirst to point to the first SQ chunk.
		__atomic_store_n(&head->sqFirst, numChunks | kNextPresent, __ATOMIC_RELEASE);

		// Initialize SQ state.
		_sqCurrentChunk = numChunks;
		_sqCurrentProgress = 0;
		_sqTailChunk = totalChunks - 1;
	}
}

bool IpcQueue::validSize(size_t size) {
	return sizeof(ElementStruct) + size <= _chunkSize;
}

coroutine<void> IpcQueue::submit(QueueSource *source, uintptr_t context) {
	assert(currentIpl() == ipl::passiveWork);

	size_t length = 0;
	for(const QueueSource *sgSource = source; sgSource; sgSource = sgSource->link)
		length += (sgSource->size + 7) & ~size_t(7);
	assert(length <= _chunkSize);

	co_await _cqMutex.async_lock();
	frg::unique_lock submitLock{frg::adopt_lock, _cqMutex};

	auto head = _mapping.access<QueueStruct>(0);

	// Get the initial CQ chunk.
	if (!_haveCqChunk) {
		auto cqFirst = __atomic_load_n(&head->cqFirst, __ATOMIC_ACQUIRE);
		while(!(cqFirst & kNextPresent)) {
			co_await _cqEvent.async_wait_if([&] () -> bool {
				__atomic_fetch_and(&head->kernelNotify, ~kKernelNotifySupplyCqChunks, __ATOMIC_ACQUIRE);
				cqFirst = __atomic_load_n(&head->cqFirst, __ATOMIC_ACQUIRE);
				return !(cqFirst & kNextPresent);
			});
		}
		_currentChunk = cqFirst & ~kNextPresent;
		_haveCqChunk = true;
	}

	size_t chunkOffset = _chunkOffsets[_currentChunk];
	auto chunkHead = _mapping.access<ChunkStruct>(chunkOffset);

	// Check if we need to move to the next chunk.
	if(static_cast<size_t>(_currentProgress) + length > _chunkSize) {
		// Wait for next chunk to become available.
		auto nextWord = __atomic_load_n(&chunkHead->next, __ATOMIC_ACQUIRE);
		while (!(nextWord & kNextPresent)) {
			co_await _cqEvent.async_wait_if([&] () -> bool {
				__atomic_fetch_and(&head->kernelNotify, ~kKernelNotifySupplyCqChunks, __ATOMIC_ACQUIRE);
				nextWord = __atomic_load_n(&chunkHead->next, __ATOMIC_ACQUIRE);
				return !(nextWord & kNextPresent);
			});
		}

		// Mark current chunk as done.
		__atomic_store_n(&chunkHead->progressFutex, _currentProgress | kProgressDone, __ATOMIC_RELEASE);

		// Signal userspace.
		auto userNotify = __atomic_fetch_or(&head->userNotify, kUserNotifyCqProgress, __ATOMIC_RELEASE);
		if(!(userNotify & kUserNotifyCqProgress))
			_userEvent.raise();

		_currentChunk = nextWord & ~kNextPresent;
		_currentProgress = 0;
		chunkOffset = _chunkOffsets[_currentChunk];
		chunkHead = _mapping.access<ChunkStruct>(chunkOffset);
	}

	ElementStruct element{
		.length = static_cast<unsigned int>(length),
		.context = reinterpret_cast<void *>(context),
	};
	auto elementOffset = offsetof(ChunkStruct, buffer) + _currentProgress;
	memcpy(_mapping.bytes_data(chunkOffset + elementOffset), &element, sizeof(ElementStruct));

	size_t sgOffset = sizeof(ElementStruct);
	for(const QueueSource *sgSource = source; sgSource; sgSource = sgSource->link) {
		memcpy(_mapping.bytes_data(chunkOffset + elementOffset + sgOffset),
				sgSource->pointer, sgSource->size);
		sgOffset += (sgSource->size + 7) & ~size_t(7);
	}

	// Advance the chunk's progress.
	__atomic_store_n(
		&chunkHead->progressFutex,
		_currentProgress + sizeof(ElementStruct) + length,
		__ATOMIC_RELEASE
	);

	// Signal userspace.
	auto userNotify = __atomic_fetch_or(&head->userNotify, kUserNotifyCqProgress, __ATOMIC_RELEASE);
	if(!(userNotify & kUserNotifyCqProgress))
		_userEvent.raise();

	_currentProgress += sizeof(ElementStruct) + length;
}

void IpcQueue::processSq() {
	assert(currentIpl() == ipl::passive);

	// Note that we clear kKernelNotifySqProgress only once we have processed all SQ elements;
	// thus, we can skip all logic unless userspace has posted new SQ elements.
	// This is an optimization that is irrelevant for correctness, hence relaxed ordering is enough.
	auto head = _mapping.access<QueueStruct>(0);
	auto notify = __atomic_fetch_and(&head->kernelNotify, ~kKernelNotifySqProgress, __ATOMIC_RELAXED);
	if (!(notify & kKernelNotifySqProgress))
		return;

	if(!_numSqChunks)
		return;

	if (!_sqMutex.try_lock())
		Thread::asyncBlockCurrent(_sqMutex.async_lock(), getCurrentThread()->mainWorkQueue().get());
	frg::unique_lock lock{frg::adopt_lock, _sqMutex};

	// Process SQ elements.
	while(true) {
		auto chunkOffset = _chunkOffsets[_sqCurrentChunk];
		auto chunkHead = _mapping.access<ChunkStruct>(chunkOffset);

		auto progressWord = __atomic_load_n(&chunkHead->progressFutex, __ATOMIC_ACQUIRE);
		auto progress = progressWord & kProgressMask;

		// Process all available elements.
		while(_sqCurrentProgress < static_cast<int>(progress)) {
			ElementStruct element;
			auto elementOffset = offsetof(ChunkStruct, buffer) + _sqCurrentProgress;
			memcpy(&element, _mapping.bytes_data(chunkOffset + elementOffset), sizeof(ElementStruct));

			// Dispatch the SQ element.
			auto dataOffset = chunkOffset + elementOffset + sizeof(ElementStruct);
			submitFromSq(selfPtr.lock(), element.opcode,
					{_mapping.bytes_data(dataOffset), element.length},
					reinterpret_cast<uintptr_t>(element.context));

			_sqCurrentProgress += sizeof(ElementStruct) + element.length;
		}

		// Check if the chunk is done.
		if(progressWord & kProgressDone) {
			auto nextWord = __atomic_load_n(&chunkHead->next, __ATOMIC_ACQUIRE);
			if(!(nextWord & kNextPresent)) {
				break; // No more chunks available.
			}

			// Recycle the processed chunk by appending it to sqFirst.
			// Reset the chunk first.
			__atomic_store_n(&chunkHead->next, 0, __ATOMIC_RELEASE);
			__atomic_store_n(&chunkHead->progressFutex, 0, __ATOMIC_RELEASE);

			// Link it to the tail of the chain.
			auto tailChunkOffset = _chunkOffsets[_sqTailChunk];
			auto tailChunkHead = _mapping.access<ChunkStruct>(tailChunkOffset);
			__atomic_store_n(&tailChunkHead->next, _sqCurrentChunk | kNextPresent, __ATOMIC_RELEASE);
			_sqTailChunk = _sqCurrentChunk;

			// Signal that chunks have been appended to the SQ.
			auto userNotify = __atomic_fetch_or(&head->userNotify,
					kUserNotifySupplySqChunks, __ATOMIC_RELEASE);
			if(!(userNotify & kUserNotifySupplySqChunks)) {
				_userEvent.raise();
			}

			_sqCurrentChunk = nextWord & ~kNextPresent;
			_sqCurrentProgress = 0;
			continue;
		}

		// Stop if kHelNotifySqProgress is clear.
		// Note that the progressFutex read happens before this (due to acquire on the progressFutex read).
		notify = __atomic_load_n(&head->kernelNotify, __ATOMIC_RELAXED);
		if (!(notify & kKernelNotifySqProgress)) {
			break;
		} else {
			// Otherwise, clear it and check progressFutex again.
			// This happens before the next progressFutex wait due to acquire.
			notify = __atomic_fetch_and(&head->kernelNotify, ~kKernelNotifySqProgress, __ATOMIC_ACQUIRE);
			// Note that no concurrent thread will clear the bit.
			// !(notify & kKernelNotifySqProgress) would be a protocol violation.
		}
	}
}

} // namespace thor

