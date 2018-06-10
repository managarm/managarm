
#include "core.hpp"
#include "work-queue.hpp"

namespace thor {

WorkQueue *WorkQueue::localQueue() {
	auto context = localExecutorContext();
	assert(context);
	return context->associatedWorkQueue;
}

// TODO: Optimize for the case where we are on the correct thread.
void WorkQueue::post(Worklet *worklet) {
	auto wq = worklet->_workQueue;
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&wq->_mutex);

	auto was_empty = wq->_posted.empty();
	wq->_posted.push_back(worklet);
	wq->_anyPosted.store(true, std::memory_order_relaxed);

	lock.unlock();
	irq_lock.unlock();

	if(was_empty)
		wq->wakeup();
}

bool WorkQueue::check() {
	return !_pending.empty() || _anyPosted.load(std::memory_order_relaxed);
}

void WorkQueue::run() {
	if(_anyPosted.load(std::memory_order_relaxed)) {
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);
		
		_pending.splice(_pending.end(), _posted);
		_anyPosted.store(false, std::memory_order_relaxed);
	}

	while(!_pending.empty()) {
		auto worklet = _pending.pop_front();
		worklet->_run(worklet);
	}
}

} // namespace thor

