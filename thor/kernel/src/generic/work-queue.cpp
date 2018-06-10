
#include "core.hpp"
#include "work-queue.hpp"

namespace thor {

void WorkQueue::post(Worklet *worklet) {
	auto wq = worklet->_workQueue;
	auto was_empty = wq->_pending.empty();
	wq->_pending.push_back(worklet);
	if(was_empty)
		wq->wakeup();
}

bool WorkQueue::check() {
	return !_pending.empty();
}

void WorkQueue::run() {
	while(!_pending.empty()) {
		auto worklet = _pending.pop_front();
		worklet->_run(worklet);
	}
}

} // namespace thor

