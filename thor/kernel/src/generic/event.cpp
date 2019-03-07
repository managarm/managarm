
#include "event.hpp"

namespace thor {

BitsetEvent::BitsetEvent()
: _currentSequence{1} {
	for(int i = 0; i < 32; i++)
		_lastTrigger[i] = 0;
}

void BitsetEvent::trigger(uint32_t bits) {
	if(!bits)
		return; // TODO: Return an error!

	auto lock = frigg::guard(&_mutex);

	_currentSequence++;
	for(int i = 0; i < 32; i++)
		if(bits & (1 << i))
			_lastTrigger[i] = _currentSequence;

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->_error = kErrSuccess;
		node->_sequence = _currentSequence;
		node->_bitset = bits;
		WorkQueue::post(node->_awaited);
	}
}

void BitsetEvent::submitAwait(AwaitBitsetNode *node, uint64_t sequence) {
	auto lock = frigg::guard(&_mutex);

	assert(sequence <= _currentSequence);
	if(sequence < _currentSequence) {
		uint32_t bits = 0;
		for(int i = 0; i < 32; i++)
			if(_lastTrigger[i] > sequence)
				bits |= 1 << i;
		assert(bits);

		node->_error = kErrSuccess;
		node->_sequence = _currentSequence;
		node->_bitset = bits;
		WorkQueue::post(node->_awaited);
	}else{
		_waitQueue.push_back(node);
	}
}

} // namespace thor

