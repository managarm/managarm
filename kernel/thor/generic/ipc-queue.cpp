
#include <string.h>

#include <frg/container_of.hpp>
#include <frigg/debug.hpp>
#include <thor-internal/core.hpp>
#include <thor-internal/ipc-queue.hpp>

namespace thor {

// ----------------------------------------------------------------------------
// IpcQueue
// ----------------------------------------------------------------------------

IpcQueue::IpcQueue(smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer,
		unsigned int size_shift, size_t)
: _space{frigg::move(space)}, _pointer{pointer}, _sizeShift{size_shift},
		_nextIndex{0},
		_currentChunk{nullptr}, _currentProgress{0},
		_chunks{*kernelAlloc} {
	_chunks.resize(1 << _sizeShift);

	async::detach_with_allocator(*kernelAlloc, _runQueue());
}

bool IpcQueue::validSize(size_t size) {
	// TODO: Note that the chunk size is currently hardcoded.
	return sizeof(ElementStruct) + size <= 4096;
}

void IpcQueue::setupChunk(size_t index, smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(index < _chunks.size());
	assert(&_chunks[index] != _currentChunk);
	_chunks[index] = Chunk{frigg::move(space), pointer};
}

void IpcQueue::submit(IpcNode *node) {
	{
		auto irqLock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		assert(!node->_queueNode.in_list);
		node->_queue = this;
		_nodeQueue.push_back(node);
	}

	_doorbell.raise();
}

coroutine<void> IpcQueue::_runQueue() {
	AddressSpaceLockHandle queueLock{_space, _pointer, sizeof(QueueStruct)
			+ (size_t{1} << _sizeShift) * sizeof(int)};
	co_await queueLock.acquire();

	while(true) {
		co_await _doorbell.async_wait_if([&] () -> bool {
			auto irqLock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&_mutex);

			return _nodeQueue.empty();
		});

		if(!_currentChunk) {
			DirectSpaceAccessor<int> accessor{queueLock, offsetof(QueueStruct, headFutex)};

			// Wait until the futex advances past _nextIndex.
			while(true) {
				bool gotNextIndex = false;
				auto futex = __atomic_load_n(accessor.get(), __ATOMIC_ACQUIRE);
				do {
					if(_nextIndex != (futex & kHeadMask)) {
						gotNextIndex = true;
						break;
					}

					// TODO: Contract violation errors should be reported to user-space.
					assert(futex == _nextIndex);
				} while(!__atomic_compare_exchange_n(accessor.get(), &futex,
						_nextIndex | kHeadWaiters, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));
				if(gotNextIndex)
					break;

				auto fa = reinterpret_cast<Address>(_pointer) + offsetof(QueueStruct, headFutex);
				co_await _space->futexSpace.wait(fa, [&] {
					return __atomic_load_n(accessor.get(), __ATOMIC_RELAXED)
							== (_nextIndex | kHeadWaiters);
				});
			}

			// Lock the chunk.
			{
				auto irqLock = frigg::guard(&irqMutex());
				auto lock = frigg::guard(&_mutex);

				size_t iq = + _nextIndex & ((size_t{1} << _sizeShift) - 1);
				size_t cn = queueLock.read<int>(offsetof(QueueStruct, indexQueue) + iq * sizeof(int));
				assert(cn < _chunks.size());
				assert(_chunks[cn].space);

				_currentChunk = &_chunks[cn];
				_nextIndex = ((_nextIndex + 1) & kHeadMask);
				_chunkLock = AddressSpaceLockHandle{_currentChunk->space,
						_currentChunk->pointer, sizeof(ChunkStruct)};
			}
			co_await _chunkLock.acquire();
		}

		// Check if there is enough space in the current chunk.
		uintptr_t dest = 0;
		size_t length = 0;
		bool chunkExhausted = false;
		{
			auto irqLock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&_mutex);

			dest = reinterpret_cast<Address>(_currentChunk->pointer)
					+ offsetof(ChunkStruct, buffer) + _currentProgress;
			assert(!(dest & 0x7));

			for(auto source = _nodeQueue.front()->_source; source; source = source->link)
				length += (source->size + 7) & ~size_t(7);
			assert(length <= _currentChunk->bufferSize);

			// Check if we need to retire the current chunk.
			if(_currentProgress + length > _currentChunk->bufferSize) {
				chunkExhausted = true;
			}
		}

		if(chunkExhausted) {
			_wakeProgressFutex(true);
			_chunkLock = AddressSpaceLockHandle{};
			_currentChunk = nullptr;
			_currentProgress = 0;
			continue;
		}

		AddressSpaceLockHandle elementLock{_currentChunk->space,
				reinterpret_cast<void *>(dest), sizeof(ElementStruct) + length};
		co_await elementLock.acquire();

		IpcNode *node;
		{
			auto irqLock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&_mutex);

			node = _nodeQueue.front();

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

			elementLock = AddressSpaceLockHandle{};
			_nodeQueue.pop_front();

			// Update the chunk progress futex.
			_currentProgress += sizeof(ElementStruct) + length;
			_wakeProgressFutex(false);
		}

		node->complete();
	}
}

void IpcQueue::_wakeProgressFutex(bool done) {
	auto progress = _currentProgress;
	if(done)
		progress |= kProgressDone;

	DirectSpaceAccessor<ChunkStruct> accessor{_chunkLock, 0};

	auto futex = __atomic_exchange_n(&accessor.get()->progressFutex,
			progress, __ATOMIC_RELEASE);

	// If user-space modifies any non-flags field, that's a contract violation.
	// TODO: Shut down the queue in this case.

	if(futex & kProgressWaiters) {
		auto fa = reinterpret_cast<Address>(_currentChunk->pointer)
				+ offsetof(ChunkStruct, progressFutex);
		_currentChunk->space->futexSpace.wake(fa);
	}
}

} // namespace thor

