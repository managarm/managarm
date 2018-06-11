
#include <string.h>

#include <frg/container_of.hpp>
#include <frigg/debug.hpp>
#include "ipc-queue.hpp"
#include "kernel.hpp"

namespace thor {

// ----------------------------------------------------------------------------
// UserQueue
// ----------------------------------------------------------------------------

UserQueue::UserQueue(frigg::SharedPtr<AddressSpace> space, void *pointer)
: _space{frigg::move(space)}, _pointer{pointer},
		_waitInFutex{false},
		_currentChunk{nullptr}, _currentProgress{0}, _nextIndex{0},
		_chunks{*kernelAlloc} {
	_queuePin = ForeignSpaceAccessor{_space, _pointer, sizeof(QueueStruct)};
	_acquireNode.setup(nullptr);
	auto acq = _queuePin.acquire(&_acquireNode);
	assert(acq);
	_queueAccessor = DirectSpaceAccessor<QueueStruct>{_queuePin, 0};

	// TODO: Take this as a constructor parameter.
	_sizeShift = _queueAccessor.get()->sizeShift;

	_chunks.resize(1 << _sizeShift);
}

void UserQueue::setupChunk(size_t index, frigg::SharedPtr<AddressSpace> space, void *pointer) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(index < _chunks.size());
	assert(&_chunks[index] != _currentChunk);
	_chunks[index] = Chunk{frigg::move(space), pointer};
}

void UserQueue::submit(QueueNode *node) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	struct Ops {
		static void ready(Worklet *worklet) {
			auto node = frg::container_of(worklet, &QueueNode::_worklet);
			auto self = node->_queue;
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			self->_progress();
		}
	};

	assert(!node->_queueNode.in_list);
	node->_queue = this;
	node->_worklet.setup(&Ops::ready, node->_wq);

	auto was_empty = _nodeQueue.empty();
	_nodeQueue.push_back(node);
	if(was_empty)
		_progress();
}

void UserQueue::_progress() {
	while(true) {
		assert(!_waitInFutex);
		assert(!_nodeQueue.empty());

		// Advance the queue if necessary.
		if(!_currentChunk) {
			_advanceChunk();
			if(_waitInFutex)
				return;
		}

		// Check if we have to retire the current chunk.
		size_t length = 0;
		for(auto source = _nodeQueue.front()->_source; source; source = source->link)
			length += (source->size + 7) & ~size_t(7);

		if(_currentProgress + length > _currentChunk->bufferSize) {
			_retireChunk();
			continue;
		}

		// Emit the next element to the current chunk.
		auto node = _nodeQueue.pop_front();

		auto dest = reinterpret_cast<Address>(_currentChunk->pointer)
				+ offsetof(ChunkStruct, buffer) + _currentProgress;
		assert(!(dest & 0x7));
		auto accessor = ForeignSpaceAccessor{_currentChunk->space,
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

		if(!_nodeQueue.empty())
			WorkQueue::post(&_nodeQueue.front()->_worklet);
		return;
	}
}

void UserQueue::_advanceChunk() {
	assert(!_currentChunk);

	if(_waitHeadFutex())
		return;

	auto source = reinterpret_cast<Address>(_pointer) + offsetof(QueueStruct, indexQueue)
			+ (_nextIndex & ((1 << _sizeShift) - 1)) * sizeof(int);
	auto accessor = ForeignSpaceAccessor{_space,
			reinterpret_cast<void *>(source), sizeof(int)};
	_acquireNode.setup(nullptr);
	auto acq = accessor.acquire(&_acquireNode);
	assert(acq);
	size_t cn = accessor.read<int>(0);
	assert(cn < _chunks.size());
	assert(_chunks[cn].space);

	_currentChunk = &_chunks[cn];
	_nextIndex = ((_nextIndex + 1) & kHeadMask);
	_chunkPin = ForeignSpaceAccessor{_currentChunk->space,
			_currentChunk->pointer, sizeof(ChunkStruct)};
	_acquireNode.setup(nullptr);
	auto acq_chunk = _chunkPin.acquire(&_acquireNode);
	assert(acq_chunk);
	_chunkAccessor = DirectSpaceAccessor<ChunkStruct>{_chunkPin, 0};
}

void UserQueue::_retireChunk() {
	assert(_currentChunk);

	_wakeProgressFutex(true);

	_chunkAccessor = DirectSpaceAccessor<ChunkStruct>{};
	_chunkPin = ForeignSpaceAccessor{};
	_currentChunk = nullptr;
	_currentProgress = 0;
}

bool UserQueue::_waitHeadFutex() {
	struct Ops {
		static void woken(Worklet *worklet) {
			auto self = frg::container_of(worklet, &UserQueue::_worklet);
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&self->_mutex);

			self->_waitInFutex = false;
			self->_progress();
		}
	};
	
	auto node = _nodeQueue.front();

	while(true) {
		auto futex = __atomic_load_n(&_queueAccessor.get()->headFutex, __ATOMIC_ACQUIRE);
		do {
			if(_nextIndex != (futex & kHeadMask))
				return false;

			// TODO: Contract violation errors should be reported to user-space.
			assert(futex == _nextIndex);
		} while(!__atomic_compare_exchange_n(&_queueAccessor.get()->headFutex, &futex,
				_nextIndex | kHeadWaiters, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE));

		auto fa = reinterpret_cast<Address>(_pointer) + offsetof(QueueStruct, headFutex);
		_worklet.setup(&Ops::woken, node->_wq);
		_futex.setup(&_worklet);
		_waitInFutex = _space->futexSpace.checkSubmitWait(fa, [&] {
			return __atomic_load_n(&_queueAccessor.get()->headFutex, __ATOMIC_RELAXED)
					== (_nextIndex | kHeadWaiters);
		}, &_futex);

		if(_waitInFutex)
			return true;
	}
}

void UserQueue::_wakeProgressFutex(bool done) {
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

