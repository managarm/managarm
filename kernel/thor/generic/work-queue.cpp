
#include <thor-internal/core.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

WorkScope::WorkScope(WorkQueue *queue)
: _scopedQueue{queue}, _outerQueue{nullptr} {
	auto context = localExecutorContext();
	_outerQueue = context->associatedWorkQueue;
	context->associatedWorkQueue = _scopedQueue;
}

WorkScope::~WorkScope() {
	auto context = localExecutorContext();
	assert(context->associatedWorkQueue == _scopedQueue);
	context->associatedWorkQueue = _outerQueue;
}

WorkQueue *WorkQueue::generalQueue() {
	auto cpuData = getCpuData();
	assert(cpuData->generalWorkQueue);
	return cpuData->generalWorkQueue.get();
}

WorkQueue *WorkQueue::localQueue() {
	auto context = localExecutorContext();
	assert(context);
	return context->associatedWorkQueue;
}

// TODO: Optimize for the case where we are on the correct thread.
void WorkQueue::post(Worklet *worklet) {
	auto wq = worklet->_workQueue;
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&wq->_mutex);

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
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);
		
		_pending.splice(_pending.end(), _posted);
		_anyPosted.store(false, std::memory_order_relaxed);
	}

	// Keep this shared pointer to avoid destructing *this here.
	smarter::shared_ptr<WorkQueue> self;
	while(!_pending.empty()) {
		auto worklet = _pending.pop_front();
		self = std::move(worklet->_workQueue);
		worklet->_run(worklet);
	}
}

} // namespace thor

