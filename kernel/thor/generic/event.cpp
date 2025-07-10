#include <thor-internal/cpu-data.hpp>
#include <thor-internal/event.hpp>
#include <thor-internal/kernel-locks.hpp>

namespace thor {

//---------------------------------------------------------------------------------------
// OneshotEvent implementation.
//---------------------------------------------------------------------------------------

void OneshotEvent::trigger() {
	assert(!_triggered); // TODO: Return an error!

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	_triggered = true;

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->error_ = Error::success;
		node->sequence_ = 2;
		node->bitset_ = 1;
		if(node->cancelCb_.try_reset())
			WorkQueue::post(node->awaited_);
	}
}

void OneshotEvent::submitAwait(AwaitEventNode<OneshotEvent> *node, uint64_t sequence) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	assert(sequence <= 1); // TODO: Return an error.

	if(_triggered) {
		node->error_ = Error::success;
		node->sequence_ = 2;
		node->bitset_ = 1;
		WorkQueue::post(node->awaited_);
	}else{
		if(!sequence) {
			node->error_ = Error::success;
			node->sequence_ = 1;
			node->bitset_ = 0;
			WorkQueue::post(node->awaited_);
		}else{
			assert(sequence == 1);

			if(!node->cancelCb_.try_set(node->cancelToken_)) {
				node->wasCancelled_ = true;
				WorkQueue::post(node->awaited_);
				return;
			}

			_waitQueue.push_back(node);
		}
	}
}

void OneshotEvent::cancelAwait(AwaitEventNode<OneshotEvent> *node) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	node->wasCancelled_ = true;
	_waitQueue.erase(_waitQueue.iterator_to(node));
	WorkQueue::post(node->awaited_);
}

//---------------------------------------------------------------------------------------
// BitsetEvent implementation.
//---------------------------------------------------------------------------------------

BitsetEvent::BitsetEvent()
: _currentSequence{1} {
	for(int i = 0; i < 32; i++)
		_lastTrigger[i] = 0;
}

void BitsetEvent::trigger(uint32_t bits) {
	if(!bits)
		return; // TODO: Return an error!

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	_currentSequence++;
	for(int i = 0; i < 32; i++)
		if(bits & (1 << i))
			_lastTrigger[i] = _currentSequence;

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->error_ = Error::success;
		node->sequence_ = _currentSequence;
		node->bitset_ = bits;
		if(node->cancelCb_.try_reset())
			WorkQueue::post(node->awaited_);
	}
}

void BitsetEvent::submitAwait(AwaitEventNode<BitsetEvent> *node, uint64_t sequence) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	assert(sequence <= _currentSequence);
	if(sequence < _currentSequence) {
		uint32_t bits = 0;
		for(int i = 0; i < 32; i++)
			if(_lastTrigger[i] > sequence)
				bits |= 1 << i;
		assert(!sequence || bits);

		node->error_ = Error::success;
		node->sequence_ = _currentSequence;
		node->bitset_ = bits;
		WorkQueue::post(node->awaited_);
	}else{
		if(!node->cancelCb_.try_set(node->cancelToken_)) {
			node->wasCancelled_ = true;
			WorkQueue::post(node->awaited_);
			return;
		}

		_waitQueue.push_back(node);
	}
}

void BitsetEvent::cancelAwait(AwaitEventNode<BitsetEvent> *node) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	node->wasCancelled_ = true;
	_waitQueue.erase(_waitQueue.iterator_to(node));
	WorkQueue::post(node->awaited_);
}

} // namespace thor

