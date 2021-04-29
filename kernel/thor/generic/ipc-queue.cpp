
#include <string.h>

#include <frg/container_of.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipc-queue.hpp>

namespace thor {

// ----------------------------------------------------------------------------
// IpcQueue
// ----------------------------------------------------------------------------

IpcQueue::IpcQueue(unsigned int ringShift, unsigned int numChunks, size_t chunkSize)
: _ringShift{ringShift}, _chunkSize{chunkSize}, _chunkOffsets{*kernelAlloc},
		_currentIndex{0}, _currentProgress{0}, _anyNodes{false} {
	auto chunksOffset = (sizeof(QueueStruct) + (sizeof(int) << ringShift) + 63) & ~size_t(63);
	auto reservedPerChunk = (sizeof(ChunkStruct) + chunkSize + 63) & ~size_t(63);
	auto overallSize = chunksOffset + numChunks * reservedPerChunk;

	// Setup internal state.
	_memory = smarter::allocate_shared<ImmediateMemory>(*kernelAlloc, overallSize);
	_memory->selfPtr = _memory;
	_chunkOffsets.resize(numChunks);
	for(unsigned int i = 0; i < numChunks; ++i)
		_chunkOffsets[i] = chunksOffset + i * reservedPerChunk;

	async::detach_with_allocator(*kernelAlloc, _runQueue());
}

bool IpcQueue::validSize(size_t size) {
	return sizeof(ElementStruct) + size <= _chunkSize;
}

void IpcQueue::submit(IpcNode *node) {
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(!node->_queueNode.in_list);
		node->_queue = this;
		_nodeQueue.push_back(node);
		_anyNodes.store(true, std::memory_order_relaxed);
	}

	_doorbell.raise();
}

coroutine<void> IpcQueue::_runQueue() {
	auto head = _memory->accessImmediate<QueueStruct>(0);

	while(true) {
		co_await _doorbell.async_wait_if([&] () -> bool {
			return !_anyNodes.load(std::memory_order_relaxed);
		});
		if(!_anyNodes.load(std::memory_order_relaxed))
			continue;

		// Wait until the futex advances past _currentIndex.
		while(true) {
			bool pastCurrentChunk = false;
			auto headFutexWord = __atomic_load_n(&head->headFutex, __ATOMIC_ACQUIRE);
			// TODO: Contract violation errors should be reported to user-space.
			assert(!(headFutexWord & ~(kHeadMask | kHeadWaiters)));
			do {
				if(_currentIndex != (headFutexWord & kHeadMask)) {
					pastCurrentChunk = true;
					break;
				}

				if(headFutexWord & kHeadWaiters)
					break; // Waiters bit is already set (in a previous iteration).
			} while(!__atomic_compare_exchange_n(&head->headFutex, &headFutexWord,
					_currentIndex | kHeadWaiters, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));
			if(pastCurrentChunk)
				break;

			auto hfOffset = offsetof(QueueStruct, headFutex);
			co_await getGlobalFutexRealm()->wait(_memory->getImmediateFutex(hfOffset),
					_currentIndex | kHeadWaiters);
		}

		// Lock the chunk.
		size_t chunkOffset;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			size_t iq = + _currentIndex & ((size_t{1} << _ringShift) - 1);
			size_t cn = *_memory->accessImmediate<int>(offsetof(QueueStruct, indexQueue) + iq * sizeof(int));
			assert(cn < _chunkOffsets.size());
			chunkOffset = _chunkOffsets[cn];
		}

		auto chunkHead = _memory->accessImmediate<ChunkStruct>(chunkOffset);

		// This inner loop runs until the chunk is exhausted.
		while(true) {
			co_await _doorbell.async_wait_if([&] () -> bool {
				return !_anyNodes.load(std::memory_order_relaxed);
			});
			if(!_anyNodes.load(std::memory_order_relaxed))
				continue;

			// Check if there is enough space in the current chunk.
			IpcNode *node;
			uintptr_t progress;
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_mutex);

				assert(!_nodeQueue.empty());
				node = _nodeQueue.front();
				progress = _currentProgress;
			}

			// Compute the overall length of the element.
			size_t length = 0;
			for(auto sgSource = _nodeQueue.front()->_source; sgSource; sgSource = sgSource->link)
				length += (sgSource->size + 7) & ~size_t(7);
			assert(length <= _chunkSize);

			// Check if we need to retire the current chunk.
			bool emitElement = true;
			if(progress + length <= _chunkSize) {
				// Emit the next element to the current chunk.
				auto elementOffset = offsetof(ChunkStruct, buffer) + _currentProgress;
				assert(!(elementOffset & 0x7));

				ElementStruct element;
				memset(&element, 0, sizeof(element));
				element.length = length;
				element.context = reinterpret_cast<void *>(node->_context);
				_memory->writeImmediate(chunkOffset + elementOffset,
						&element, sizeof(ElementStruct));

				size_t sgOffset = sizeof(ElementStruct);
				for(auto sgSource = node->_source; sgSource; sgSource = sgSource->link) {
					_memory->writeImmediate(chunkOffset + elementOffset + sgOffset,
							sgSource->pointer, sgSource->size);
					sgOffset += (sgSource->size + 7) & ~size_t(7);
				}
			}else{
				emitElement = false;
			}

			// Update the progress futex.
			unsigned int newProgressWord;
			if(emitElement) {
				newProgressWord = progress + sizeof(ElementStruct) + length;
			}else{
				newProgressWord = progress | kProgressDone;
			}

			auto progressFutexWord = __atomic_exchange_n(&chunkHead->progressFutex,
					newProgressWord, __ATOMIC_RELEASE);
			// If user-space modifies any non-flags field, that's a contract violation.
			// TODO: Shut down the queue in this case.
			if(progressFutexWord & kProgressWaiters) {
				auto pfOffset = chunkOffset + offsetof(ChunkStruct, progressFutex);
				getGlobalFutexRealm()->wake(_memory->resolveImmediateFutex(pfOffset));
			}

			// Update our internal state and retire the chunk.
			if(!emitElement) {
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_mutex);

				_currentIndex = ((_currentIndex + 1) & kHeadMask);
				_currentProgress = 0;
				break;
			}

			// Update our internal state and retire the node.
			{
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_mutex);

				_currentProgress += sizeof(ElementStruct) + length;
				_nodeQueue.pop_front();

				assert(_anyNodes.load(std::memory_order_relaxed));
				if(_nodeQueue.empty())
					_anyNodes.store(false, std::memory_order_relaxed);
			}

			node->complete();
		}
	}
}

} // namespace thor

