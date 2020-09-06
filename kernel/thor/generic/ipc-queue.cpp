
#include <string.h>

#include <frg/container_of.hpp>
#include <thor-internal/core.hpp>
#include <thor-internal/ipc-queue.hpp>

namespace thor {

// ----------------------------------------------------------------------------
// IpcQueue
// ----------------------------------------------------------------------------

IpcQueue::IpcQueue(smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer,
		unsigned int size_shift, size_t)
: _space{std::move(space)}, _pointer{pointer}, _sizeShift{size_shift},
		_chunks{*kernelAlloc},
		_currentIndex{0}, _currentProgress{0}, _anyNodes{false} {
	_chunks.resize(1 << _sizeShift);

	async::detach_with_allocator(*kernelAlloc, _runQueue());
}

bool IpcQueue::validSize(size_t size) {
	// TODO: Note that the chunk size is currently hardcoded.
	return sizeof(ElementStruct) + size <= 4096;
}

void IpcQueue::setupChunk(size_t index, smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	assert(index < _chunks.size());
	_chunks[index] = Chunk{std::move(space), pointer};
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
	AddressSpaceLockHandle queueLock{_space, _pointer, sizeof(QueueStruct)
			+ (size_t{1} << _sizeShift) * sizeof(int)};
	co_await queueLock.acquire(WorkQueue::generalQueue()->take());

	while(true) {
		co_await _doorbell.async_wait_if([&] () -> bool {
			return !_anyNodes.load(std::memory_order_relaxed);
		});
		if(!_anyNodes.load(std::memory_order_relaxed))
			continue;

		// Wait until the futex advances past _currentIndex.
		DirectSpaceAccessor<int> headFutexAccessor{queueLock, offsetof(QueueStruct, headFutex)};
		while(true) {
			bool pastCurrentChunk = false;
			auto headFutexWord = __atomic_load_n(headFutexAccessor.get(), __ATOMIC_ACQUIRE);
			do {
				if(_currentIndex != (headFutexWord & kHeadMask)) {
					pastCurrentChunk = true;
					break;
				}

				// TODO: Contract violation errors should be reported to user-space.
				assert(headFutexWord == _currentIndex);
			} while(!__atomic_compare_exchange_n(headFutexAccessor.get(), &headFutexWord,
					_currentIndex | kHeadWaiters, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));
			if(pastCurrentChunk)
				break;

			auto fa = reinterpret_cast<Address>(_pointer) + offsetof(QueueStruct, headFutex);
			co_await _space->futexSpace.wait(fa, [&] {
				return __atomic_load_n(headFutexAccessor.get(), __ATOMIC_RELAXED)
						== (_currentIndex | kHeadWaiters);
			});
		}

		// Lock the chunk.
		Chunk *currentChunk;
		{
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&_mutex);

			size_t iq = + _currentIndex & ((size_t{1} << _sizeShift) - 1);
			size_t cn = queueLock.read<int>(offsetof(QueueStruct, indexQueue) + iq * sizeof(int));
			assert(cn < _chunks.size());
			assert(_chunks[cn].space);

			currentChunk = &_chunks[cn];
		}
		AddressSpaceLockHandle chunkLock{currentChunk->space,
					currentChunk->pointer, sizeof(ChunkStruct)};
		co_await chunkLock.acquire(WorkQueue::generalQueue()->take());

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

			// Compute destion pointer and length of the element.
			auto dest = reinterpret_cast<Address>(currentChunk->pointer)
					+ offsetof(ChunkStruct, buffer) + _currentProgress;
			assert(!(dest & 0x7));

			size_t length = 0;
			for(auto source = _nodeQueue.front()->_source; source; source = source->link)
				length += (source->size + 7) & ~size_t(7);
			assert(length <= currentChunk->bufferSize);

			// Check if we need to retire the current chunk.
			bool emitElement = true;
			if(progress + length <= currentChunk->bufferSize) {
				AddressSpaceLockHandle elementLock{currentChunk->space,
						reinterpret_cast<void *>(dest), sizeof(ElementStruct) + length};
				co_await elementLock.acquire(WorkQueue::generalQueue()->take());

				// Emit the next element to the current chunk.
				ElementStruct element;
				memset(&element, 0, sizeof(element));
				element.length = length;
				element.context = reinterpret_cast<void *>(node->_context);
				auto err = elementLock.write(0, &element, sizeof(ElementStruct));
				assert(err == Error::success);

				size_t disp = sizeof(ElementStruct);
				for(auto source = node->_source; source; source = source->link) {
					err = elementLock.write(disp, source->pointer, source->size);
					assert(err == Error::success);
					disp += (source->size + 7) & ~size_t(7);
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

			DirectSpaceAccessor<ChunkStruct> chunkAccessor{chunkLock, 0};

			auto progressFutexWord = __atomic_exchange_n(&chunkAccessor.get()->progressFutex,
					newProgressWord, __ATOMIC_RELEASE);
			// If user-space modifies any non-flags field, that's a contract violation.
			// TODO: Shut down the queue in this case.
			if(progressFutexWord & kProgressWaiters) {
				auto fa = reinterpret_cast<Address>(currentChunk->pointer)
						+ offsetof(ChunkStruct, progressFutex);
				currentChunk->space->futexSpace.wake(fa);
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

