
#include "arch/x86/ints.hpp"
#include <thor-internal/event.hpp>

namespace thor {

//---------------------------------------------------------------------------------------
// OneshotEvent implementation.
//---------------------------------------------------------------------------------------

void OneshotEvent::trigger() {
	assert(!_triggered); // TODO: Return an error!

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	_triggered = true;

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->_error = Error::success;
		node->_sequence = 2;
		node->_bitset = 1;
		WorkQueue::post(node->_awaited);
	}
}

void OneshotEvent::submitAwait(AwaitEventNode *node, uint64_t sequence) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(sequence <= 1); // TODO: Return an error.

	if(_triggered) {
		node->_error = Error::success;
		node->_sequence = 2;
		node->_bitset = 1;
		WorkQueue::post(node->_awaited);
	}else{
		if(!sequence) {
			node->_error = Error::success;
			node->_sequence = 1;
			node->_bitset = 0;
			WorkQueue::post(node->_awaited);
		}else{
			assert(sequence == 1);
			_waitQueue.push_back(node);
		}
	}
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

	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	_currentSequence++;
	for(int i = 0; i < 32; i++)
		if(bits & (1 << i))
			_lastTrigger[i] = _currentSequence;

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->_error = Error::success;
		node->_sequence = _currentSequence;
		node->_bitset = bits;
		WorkQueue::post(node->_awaited);
	}
}

void BitsetEvent::submitAwait(AwaitEventNode *node, uint64_t sequence) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(sequence <= _currentSequence);
	if(sequence < _currentSequence) {
		uint32_t bits = 0;
		for(int i = 0; i < 32; i++)
			if(_lastTrigger[i] > sequence)
				bits |= 1 << i;
		assert(!sequence || bits);

		node->_error = Error::success;
		node->_sequence = _currentSequence;
		node->_bitset = bits;
		WorkQueue::post(node->_awaited);
	}else{
		_waitQueue.push_back(node);
	}
}

} // namespace thor

