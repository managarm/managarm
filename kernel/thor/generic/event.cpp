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

//---------------------------------------------------------------------------------------
// SequencedEvent implementation.
//---------------------------------------------------------------------------------------

std::expected<
	frg::tuple<
		smarter::shared_ptr<SequencedEvent, TwoPeerPolicy>,
		smarter::shared_ptr<SequencedEvent, TwoPeerPolicy>
	>,
	Error
> SequencedEvent::create() {
	auto event = allocate_rcu_shared<SequencedEvent>(*kernelAlloc, CtorToken{});
	assert(event.policy().base()->ctr().check_count() == 1);
	return adoptPeers(std::move(event));
}

void SequencedEvent::onPeersZero(int end) {
	auto wasGone = endGone_[end].exchange(true, std::memory_order_release);
	assert(!wasGone);

	if(end == raiseEnd)
		event_.raise();
}

uint64_t SequencedEvent::sequence() {
	return sequence_.load(std::memory_order_relaxed);
}

std::expected<void, Error> SequencedEvent::raise() {
	if(endGone_[waitEnd].load(std::memory_order_relaxed))
		return std::unexpected{Error::endOfLane};

	// Release pairs with the acquire in awaitEvent(); since all writes to sequence_ are RMWs,
	// they form one release sequence and observing a value synchronizes with all earlier raises.
	sequence_.fetch_add(1, std::memory_order_release);
	event_.raise();

	return {};
}

coroutine<frg::expected<Error, uint64_t>> SequencedEvent::awaitEvent(uint64_t sequence,
		async::cancellation_token cancelToken) {
	if(sequence > sequence_.load(std::memory_order_relaxed))
		co_return Error::illegalArgs;

	// raise() increments before it wakes, so we can be woken by an increment that we already saw.
	while(true) {
		auto outcome = co_await event_.async_wait_if([&] () -> bool {
			return sequence_.load(std::memory_order_relaxed) == sequence
					&& !endGone_[raiseEnd].load(std::memory_order_relaxed);
		}, cancelToken);
		if(!outcome)
			co_return Error::cancelled;

		// Acquire endGone_ first such that we see all increments of the raise end.
		// The condition above only decides whether to sleep, so it can use relaxed loads.
		auto gone = endGone_[raiseEnd].load(std::memory_order_acquire);
		auto current = sequence_.load(std::memory_order_acquire);
		if(current != sequence)
			co_return current;
		if(gone)
			co_return Error::endOfLane;
	}
}

} // namespace thor

