
#include <string.h>

#include <frg/container_of.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/ipc-queue.hpp>

namespace thor {

// ----------------------------------------------------------------------------
// IpcQueue
// ----------------------------------------------------------------------------

IpcQueue::IpcQueue(unsigned int numChunks, size_t chunkSize)
: _chunkSize{chunkSize}, _chunkOffsets{*kernelAlloc},
		_currentChunk{0}, _currentProgress{0}, _anyNodes{false} {
	auto chunksOffset = (sizeof(QueueStruct) + 63) & ~size_t(63);
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

	// Wait for the initial chunk to be supplied via cqFirst.
	{
		auto cqFirst = __atomic_load_n(&head->cqFirst, __ATOMIC_ACQUIRE);
		if(!(cqFirst & kNextPresent)) {
			while(true) {
				auto kernelNotify = __atomic_fetch_and(&head->kernelNotify,
						~kKernelNotifySupplyCqChunks, __ATOMIC_ACQUIRE);

				cqFirst = __atomic_load_n(&head->cqFirst, __ATOMIC_ACQUIRE);
				if(cqFirst & kNextPresent)
					break;

				auto knOffset = offsetof(QueueStruct, kernelNotify);
				co_await getGlobalFutexRealm()->wait(_memory->getImmediateFutex(knOffset),
						kernelNotify & ~kKernelNotifySupplyCqChunks);
			}
		}
		_currentChunk = cqFirst & ~kNextPresent;
	}

	while(true) {
		// Wait until there are nodes to process.
		co_await _doorbell.async_wait_if([&] () -> bool {
			return !_anyNodes.load(std::memory_order_relaxed);
		});
		if(!_anyNodes.load(std::memory_order_relaxed))
			continue;

		size_t chunkOffset;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);
			chunkOffset = _chunkOffsets[_currentChunk];
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
			int nextWord = 0;
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

				// Wait until the next chunk is available before setting the done bit.
				// This simplifies lifetime handling on the userspace side.
				nextWord = __atomic_load_n(&chunkHead->next, __ATOMIC_ACQUIRE);
				if(!(nextWord & kNextPresent)) {
					while(true) {
						auto kernelNotify = __atomic_fetch_and(&head->kernelNotify,
								~kKernelNotifySupplyCqChunks, __ATOMIC_ACQUIRE);

						nextWord = __atomic_load_n(&chunkHead->next, __ATOMIC_ACQUIRE);
						if(nextWord & kNextPresent)
							break;

						auto knOffset = offsetof(QueueStruct, kernelNotify);
						co_await getGlobalFutexRealm()->wait(_memory->getImmediateFutex(knOffset),
								kernelNotify & ~kKernelNotifySupplyCqChunks);
					}
				}
			}

			// Update the progress futex.
			unsigned int newProgressWord;
			if(emitElement) {
				newProgressWord = progress + sizeof(ElementStruct) + length;
			}else{
				newProgressWord = progress | kProgressDone;
			}

			__atomic_store_n(&chunkHead->progressFutex, newProgressWord, __ATOMIC_RELEASE);

			auto userNotify = __atomic_fetch_or(&head->userNotify,
					kUserNotifyCqProgress, __ATOMIC_RELEASE);
			// If user-space modifies any non-flags field, that's a contract violation.
			// TODO: Shut down the queue in this case.
			if(!(userNotify & kUserNotifyCqProgress)) {
				auto unOffset = offsetof(QueueStruct, userNotify);
				getGlobalFutexRealm()->wake(_memory->resolveImmediateFutex(unOffset), UINT32_MAX);
			}

			// Update our internal state and retire the chunk.
			if(!emitElement) {
				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&_mutex);

				_currentChunk = nextWord & ~kNextPresent;
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

