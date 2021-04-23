#include <thor-internal/core.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

WorkQueue *WorkQueue::generalQueue() {
	auto cpuData = getCpuData();
	assert(cpuData->generalWorkQueue);
	return cpuData->generalWorkQueue.get();
}

void WorkQueue::post(Worklet *worklet) {
	auto wq = worklet->_workQueue;

	bool invokeWakeup;
	if(wq->_executorContext == currentExecutorContext()) {
		auto irqLock = frg::guard(&irqMutex());

		invokeWakeup = wq->_pending.empty();
		wq->_pending.push_back(worklet);
	}else{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&wq->_mutex);

		invokeWakeup = wq->_posted.empty();
		wq->_posted.push_back(worklet);
		wq->_anyPosted.store(true, std::memory_order_relaxed);
	}

	if(invokeWakeup)
		wq->wakeup();
}

bool WorkQueue::check() {
	// _pending is only accessed from the thread/fiber that runs the WQ.
	// For _anyPosted, see the comment in the header file.
	return !_pending.empty() || _anyPosted.load(std::memory_order_relaxed);
}

void WorkQueue::run() {
	assert(_executorContext == currentExecutorContext());

	if(_anyPosted.load(std::memory_order_relaxed)) {
		auto irqLock = frg::guard(&irqMutex());
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
