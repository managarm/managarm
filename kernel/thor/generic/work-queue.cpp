#include <thor-internal/cpu-data.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

smarter::borrowed_ptr<WorkQueue> WorkQueue::generalQueue() {
	auto cpuData = getCpuData();
	assert(cpuData->generalWorkQueue);
	return cpuData->generalWorkQueue;
}

void WorkQueue::post(Worklet *worklet) {
	bool invokeWakeup;
	if(_executorContext == currentExecutorContext()) {
		// If we are not in interrupt context,
		// we can push directly to the pending queue without running into races.
		if (contextIpl() < ipl::interrupt) [[likely]] {
			auto pendingEmpty = !_pending.empty();

			if (_inRun.load(std::memory_order_relaxed)) {
				// If a worklet posts another worklet, we proceed in LIFO order,
				// i.e., in the same order that a call stack would also proceed.
				_pending.push_front(worklet);
				return;
			}
			_pending.push_back(worklet);

			if (currentIpl() <= _wqIpl) {
				run();
				return;
			}

			invokeWakeup = pendingEmpty;
		} else {
			assert(!intsAreEnabled());

			invokeWakeup = _localQueue.empty();
			_localQueue.push_back(worklet);
			_localPosted.store(true, std::memory_order_relaxed);
		}
	}else{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		invokeWakeup = _lockedQueue.empty();
		_lockedQueue.push_back(worklet);
		_lockedPosted.store(true, std::memory_order_relaxed);
	}

	if(invokeWakeup)
		wakeup();
}

// immediatelyDispatchable() only returns true if we are already in run();
// otherwise, WQ entry and exit logic in run() would be skipped.
// However, immediatelyDispatchable() is more strict than the _inRun code path in post().
// In particular, it only returns true if run() was not interrupted by anything,
// i.e., the IPL also has to be correct to continue running the WQ immediately.
bool WorkQueue::immediatelyDispatchable() {
	// Note: post() checks for contextIpl() < ipl::interrupt but that is implied by currentIpl() < _wqIpl.
	return _executorContext == currentExecutorContext()
		&& _inRun.load(std::memory_order_relaxed)
		&& currentIpl() <= _wqIpl;
}

bool WorkQueue::check() {
	// _localPosted is only accessed from the thread/fiber that runs the WQ.
	// For _lockedPosted, see the comment in the header file.
	return !_pending.empty()
			|| _localPosted.load(std::memory_order_relaxed)
			|| _lockedPosted.load(std::memory_order_relaxed);
}

void WorkQueue::run() {
	assert(_executorContext == currentExecutorContext());
	assert(!_inRun.load(std::memory_order_relaxed));

	std::atomic_signal_fence(std::memory_order_release);
	_inRun.store(true, std::memory_order_relaxed);

	auto checkLocal = _localPosted.load(std::memory_order_relaxed);
	auto checkLocked = _lockedPosted.load(std::memory_order_relaxed);
	if (checkLocal || checkLocked) {
		auto irqLock = frg::guard(&irqMutex());

		_pending.splice(_pending.end(), _localQueue);
		_localPosted.store(false, std::memory_order_relaxed);

		if(checkLocked) {
			auto lock = frg::guard(&_mutex);

			_pending.splice(_pending.end(), _lockedQueue);
			_lockedPosted.store(false, std::memory_order_relaxed);
		}
	}

	while(!_pending.empty()) {
		auto worklet = _pending.pop_front();
		worklet->_run(worklet);
	}

	std::atomic_signal_fence(std::memory_order_release);
	_inRun.store(false, std::memory_order_relaxed);
}

} // namespace thor
