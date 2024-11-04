#include <thor-internal/cpu-data.hpp>
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
	if (wq->_executorContext == currentExecutorContext()) {
		auto irqLock = frg::guard(&irqMutex());

		invokeWakeup = wq->_localQueue.empty();
		wq->_localQueue.push_back(worklet);
		wq->_localPosted.store(true, std::memory_order_relaxed);
	} else {
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&wq->_mutex);

		invokeWakeup = wq->_lockedQueue.empty();
		wq->_lockedQueue.push_back(worklet);
		wq->_lockedPosted.store(true, std::memory_order_relaxed);
	}

	if (invokeWakeup)
		wq->wakeup();
}

bool WorkQueue::enter(Worklet *worklet) {
	auto wq = worklet->_workQueue;

	bool invokeWakeup;
	if (wq->_executorContext == currentExecutorContext()) {
		// Fast-track if we are on the right executor and the WQ is being drained.
		if (wq->_inRun.load(std::memory_order_relaxed)) {
			std::atomic_signal_fence(std::memory_order_acquire);
			return true;
		}

		// Same logic as in post().
		auto irqLock = frg::guard(&irqMutex());

		invokeWakeup = wq->_localQueue.empty();
		wq->_localQueue.push_back(worklet);
		wq->_localPosted.store(true, std::memory_order_relaxed);
	} else {
		// Same logic as in post().
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&wq->_mutex);

		invokeWakeup = wq->_lockedQueue.empty();
		wq->_lockedQueue.push_back(worklet);
		wq->_lockedPosted.store(true, std::memory_order_relaxed);
	}

	if (invokeWakeup)
		wq->wakeup();
	return false;
}

bool WorkQueue::check() {
	// _localPosted is only accessed from the thread/fiber that runs the WQ.
	// For _lockedPosted, see the comment in the header file.
	return _localPosted.load(std::memory_order_relaxed) ||
	       _lockedPosted.load(std::memory_order_relaxed);
}

void WorkQueue::run() {
	assert(_executorContext == currentExecutorContext());
	assert(!_inRun.load(std::memory_order_relaxed));

	std::atomic_signal_fence(std::memory_order_release);
	_inRun.store(true, std::memory_order_relaxed);

	frg::intrusive_list<
	    Worklet,
	    frg::locate_member<Worklet, frg::default_list_hook<Worklet>, &Worklet::_hook>>
	    pending;
	{
		auto irqLock = frg::guard(&irqMutex());

		pending.splice(pending.end(), _localQueue);
		_localPosted.store(false, std::memory_order_relaxed);

		if (_lockedPosted.load(std::memory_order_relaxed)) {
			auto lock = frg::guard(&_mutex);

			pending.splice(pending.end(), _lockedQueue);
			_lockedPosted.store(false, std::memory_order_relaxed);
		}
	}

	// Keep this shared pointer to avoid destructing *this here.
	smarter::shared_ptr<WorkQueue> self;
	while (!pending.empty()) {
		auto worklet = pending.pop_front();
		self = std::move(worklet->_workQueue);
		worklet->_run(worklet);
	}

	std::atomic_signal_fence(std::memory_order_release);
	_inRun.store(false, std::memory_order_relaxed);
}

} // namespace thor
