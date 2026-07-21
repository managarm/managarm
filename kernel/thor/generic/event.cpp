#include <thor-internal/cpu-data.hpp>
#include <thor-internal/event.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/rcu.hpp>

namespace thor {

//---------------------------------------------------------------------------------------
// OneshotEvent implementation.
//---------------------------------------------------------------------------------------

std::expected<smarter::shared_ptr<OneshotEvent>, Error> OneshotEvent::create() {
	auto ptr = allocate_rcu_shared<OneshotEvent>(*kernelAlloc, CtorToken{});
	return ptr;
}

std::expected<void, Error> OneshotEvent::trigger() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(_triggered)
		return std::unexpected{Error::illegalState};

	_triggered = true;

	while(!_waitQueue.empty()) {
		auto node = _waitQueue.pop_front();
		node->error_ = Error::success;
		node->sequence_ = 2;
		node->bitset_ = 1;
		if(node->cancelCb_.try_reset())
			node->wq_->post(node->awaited_);
	}

	return {};
}

void OneshotEvent::submitAwait(AwaitEventNode<OneshotEvent> *node, uint64_t sequence) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	assert(sequence <= 1); // TODO: Return an error.

	if(_triggered) {
		node->error_ = Error::success;
		node->sequence_ = 2;
		node->bitset_ = 1;
		node->wq_->post(node->awaited_);
	}else{
		if(!sequence) {
			node->error_ = Error::success;
			node->sequence_ = 1;
			node->bitset_ = 0;
			node->wq_->post(node->awaited_);
		}else{
			assert(sequence == 1);

			if(!node->cancelCb_.try_set(node->cancelToken_)) {
				node->wasCancelled_ = true;
				node->wq_->post(node->awaited_);
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
	node->wq_->post(node->awaited_);
}

//---------------------------------------------------------------------------------------
// BitsetEvent implementation.
//---------------------------------------------------------------------------------------

std::expected<smarter::shared_ptr<BitsetEvent>, Error> BitsetEvent::create() {
	auto ptr = allocate_rcu_shared<BitsetEvent>(*kernelAlloc, CtorToken{});
	return ptr;
}

BitsetEvent::BitsetEvent(CtorToken)
: _currentSequence{1} {
	for(int i = 0; i < 32; i++)
		_lastTrigger[i] = 0;
}

std::expected<void, Error> BitsetEvent::trigger(uint32_t bits) {
	if(!bits)
		return std::unexpected{Error::illegalArgs};

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
			node->wq_->post(node->awaited_);
	}

	return {};
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
		node->wq_->post(node->awaited_);
	}else{
		if(!node->cancelCb_.try_set(node->cancelToken_)) {
			node->wasCancelled_ = true;
			node->wq_->post(node->awaited_);
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
	node->wq_->post(node->awaited_);
}

} // namespace thor

