
#include <string.h>

#include <frg/container_of.hpp>
#include <frigg/debug.hpp>
#include "ipc-queue.hpp"
#include "kernel.hpp"

namespace thor {

// ----------------------------------------------------------------------------
// IpcQueue
// ----------------------------------------------------------------------------

IpcQueue::IpcQueue(smarter::shared_ptr<AddressSpace, BindableHandle> space, void *pointer)
: _space{frigg::move(space)}, _pointer{pointer},
		_currentChunk{nullptr}, _currentProgress{0}, _nextIndex{0},
		_chunks{*kernelAlloc} {
	_queuePin = AddressSpaceLockHandle{_space, _pointer, sizeof(QueueStruct)};
	_acquireNode.setup(nullptr);
	auto acq = _queuePin.acquire(&_acquireNode);
	assert(acq);
	_queueAccessor = DirectSpaceAccessor<QueueStruct>{_queuePin, 0};

	// TODO: Take this as a constructor parameter.
	_sizeShift = _queueAccessor.get()->sizeShift;

	_chunks.resize(1 << _sizeShift);
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
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(!node->_queueNode.in_list);
	node->_queue = this;
	_nodeQueue.push_back(node);

	if(!_inProgressLoop) {
		_worklet.setup([] (Worklet *worklet) {
			auto self = frg::container_of(worklet, &IpcQueue::_worklet);
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			self->_progress();
		});
		_inProgressLoop = true;
		WorkQueue::post(&_worklet);
	}
}

void IpcQueue::_progress() {
	while(!_nodeQueue.empty()) {
		assert(_inProgressLoop);

		// Advance the queue if necessary.
		if(!_currentChunk) {
			if(!_advanceChunk())
				return;
		}

		// Check if there is enough space in the current chunk.
		size_t length = 0;
		for(auto source = _nodeQueue.front()->_source; source; source = source->link)
			length += (source->size + 7) & ~size_t(7);

		assert(length <= _currentChunk->bufferSize);

		// Check if we need to retire the current chunk.
		if(_currentProgress + length > _currentChunk->bufferSize) {
			_wakeProgressFutex(true);

			_chunkAccessor = DirectSpaceAccessor<ChunkStruct>{};
			_chunkPin = AddressSpaceLockHandle{};
			_currentChunk = nullptr;
			_currentProgress = 0;
			continue;
		}

		// Emit the next element to the current chunk.
		auto node = _nodeQueue.pop_front();

		auto dest = reinterpret_cast<Address>(_currentChunk->pointer)
				+ offsetof(ChunkStruct, buffer) + _currentProgress;
		assert(!(dest & 0x7));
		auto accessor = AddressSpaceLockHandle{_currentChunk->space,
				reinterpret_cast<void *>(dest), sizeof(ElementStruct) + length};
		_acquireNode.setup(nullptr);
		auto acq = accessor.acquire(&_acquireNode);
		assert(acq);

		ElementStruct element;
		memset(&element, 0, sizeof(element));
		element.length = length;
		element.context = reinterpret_cast<void *>(node->_context);
		auto err = accessor.write(0, &element, sizeof(ElementStruct));
		assert(!err);

		size_t disp = sizeof(ElementStruct);
		for(auto source = node->_source; source; source = source->link) {
			err = accessor.write(disp, source->pointer, source->size);
			assert(!err);
			disp += (source->size + 7) & ~size_t(7);
		}

		node->complete();

		// Update the chunk progress futex.
		_currentProgress += sizeof(ElementStruct) + length;
		_wakeProgressFutex(false);
	}

	_inProgressLoop = false;
}

// Returns true if the operation was done synchronously.
bool IpcQueue::_advanceChunk() {
	assert(!_currentChunk);

	if(!_waitHeadFutex())
		return false;

	auto source = reinterpret_cast<Address>(_pointer) + offsetof(QueueStruct, indexQueue)
			+ (_nextIndex & ((1 << _sizeShift) - 1)) * sizeof(int);
	auto accessor = AddressSpaceLockHandle{_space,
			reinterpret_cast<void *>(source), sizeof(int)};
	_acquireNode.setup(nullptr);
	auto acq = accessor.acquire(&_acquireNode);
	assert(acq);
	size_t cn = accessor.read<int>(0);
	assert(cn < _chunks.size());
	assert(_chunks[cn].space);

	_currentChunk = &_chunks[cn];
	_nextIndex = ((_nextIndex + 1) & kHeadMask);
	_chunkPin = AddressSpaceLockHandle{_currentChunk->space,
			_currentChunk->pointer, sizeof(ChunkStruct)};
	_acquireNode.setup(nullptr);
	auto acq_chunk = _chunkPin.acquire(&_acquireNode);
	assert(acq_chunk);
	_chunkAccessor = DirectSpaceAccessor<ChunkStruct>{_chunkPin, 0};
	return true;
}

// Returns true if the operation was done synchronously.
bool IpcQueue::_waitHeadFutex() {
	struct Ops {
		static void woken(Worklet *worklet) {
			auto self = frg::container_of(worklet, &IpcQueue::_worklet);
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			self->_progress();
		}
	};
	
	while(true) {
		auto futex = __atomic_load_n(&_queueAccessor.get()->headFutex, __ATOMIC_ACQUIRE);
		do {
			if(_nextIndex != (futex & kHeadMask))
				return true;

			// TODO: Contract violation errors should be reported to user-space.
			assert(futex == _nextIndex);
		} while(!__atomic_compare_exchange_n(&_queueAccessor.get()->headFutex, &futex,
				_nextIndex | kHeadWaiters, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));

		auto fa = reinterpret_cast<Address>(_pointer) + offsetof(QueueStruct, headFutex);
		_worklet.setup(&Ops::woken);
		_futex.setup(&_worklet);
		auto wait_in_futex = _space->futexSpace.checkSubmitWait(fa, [&] {
			return __atomic_load_n(&_queueAccessor.get()->headFutex, __ATOMIC_RELAXED)
					== (_nextIndex | kHeadWaiters);
		}, &_futex);

		if(wait_in_futex)
			return false;
	}
}

void IpcQueue::_wakeProgressFutex(bool done) {
	auto progress = _currentProgress;
	if(done)
		progress |= kProgressDone;
	auto futex = __atomic_exchange_n(&_chunkAccessor.get()->progressFutex,
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

