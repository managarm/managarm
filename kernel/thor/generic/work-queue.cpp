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
		auto irqLock = frg::guard(&irqMutex());

		invokeWakeup = _localQueue.empty();
		_localQueue.push_back(worklet);
		_localPosted.store(true, std::memory_order_relaxed);
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

bool WorkQueue::enter(Worklet *worklet) {
	bool invokeWakeup;
	if(_executorContext == currentExecutorContext()) {
		// Fast-track if we are on the right executor and the WQ is being drained.
		if(_inRun.load(std::memory_order_relaxed)) {
			std::atomic_signal_fence(std::memory_order_acquire);
			return true;
		}

		// Same logic as in post().
		auto irqLock = frg::guard(&irqMutex());

		invokeWakeup = _localQueue.empty();
		_localQueue.push_back(worklet);
		_localPosted.store(true, std::memory_order_relaxed);
	}else{
		// Same logic as in post().
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		invokeWakeup = _lockedQueue.empty();
		_lockedQueue.push_back(worklet);
		_lockedPosted.store(true, std::memory_order_relaxed);
	}

	if(invokeWakeup)
		wakeup();
	return false;
}

bool WorkQueue::check() {
	// _localPosted is only accessed from the thread/fiber that runs the WQ.
	// For _lockedPosted, see the comment in the header file.
	return _localPosted.load(std::memory_order_relaxed)
			|| _lockedPosted.load(std::memory_order_relaxed);
}

void WorkQueue::run() {
	assert(_executorContext == currentExecutorContext());
	assert(!_inRun.load(std::memory_order_relaxed));

	std::atomic_signal_fence(std::memory_order_release);
	_inRun.store(true, std::memory_order_relaxed);

	frg::intrusive_list<
		Worklet,
		frg::locate_member<
			Worklet,
			frg::default_list_hook<Worklet>,
			&Worklet::_hook
		>
	> pending;
	{
		auto irqLock = frg::guard(&irqMutex());

		pending.splice(pending.end(), _localQueue);
		_localPosted.store(false, std::memory_order_relaxed);

		if(_lockedPosted.load(std::memory_order_relaxed)) {
			auto lock = frg::guard(&_mutex);

			pending.splice(pending.end(), _lockedQueue);
			_lockedPosted.store(false, std::memory_order_relaxed);
		}
	}

	while(!pending.empty()) {
		auto worklet = pending.pop_front();
		worklet->_run(worklet);
	}

	std::atomic_signal_fence(std::memory_order_release);
	_inRun.store(false, std::memory_order_relaxed);
}

} // namespace thor
