
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::deferCurrent() {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;

	assert(!intsAreEnabled());
	if(forkExecutor()) {
		ScheduleGuard schedule_lock(scheduleLock.get());
		enqueueInSchedule(schedule_lock, this_thread);
		
		runDetached([] (ScheduleGuard schedule_lock) {
			doSchedule(frigg::move(schedule_lock));
		}, frigg::move(schedule_lock));
	}
}

void Thread::blockCurrent(void *, void (*) (void *)) {
	assert(!"Use blockCurrentWhile() instead");
}

void Thread::activateOther(frigg::UnsafePtr<Thread> other_thread) {
	assert(other_thread->_runState == kRunSuspended
			|| other_thread->_runState == kRunDeferred);
	other_thread->_runState = kRunActive;
}

void Thread::unblockOther(frigg::UnsafePtr<Thread> thread) {
	auto lock = frigg::guard(&thread->_mutex);
	if(thread->_runState != kRunBlocked)
		return;

	thread->_runState = kRunDeferred;
	{
		ScheduleGuard schedule_lock(scheduleLock.get());
		enqueueInSchedule(schedule_lock, thread);
	}
}

Thread::Thread(KernelSharedPtr<Universe> universe,
		KernelSharedPtr<AddressSpace> address_space)
: flags(0), _runState(kRunSuspended),
		_numTicks(0), _activationTick(0),
		_pendingSignal(kSigNone), _runCount(1),
		_context(kernelStack.base()),
		_universe(frigg::move(universe)), _addressSpace(frigg::move(address_space)) {
//	frigg::infoLogger() << "[" << globalThreadId << "] New thread!" << frigg::endLog;
	auto stream = createStream();
	_superiorLane = frigg::move(stream.get<0>());
	_inferiorLane = frigg::move(stream.get<1>());
}

Thread::~Thread() {
	assert(!"Thread destructed");
	if(!_observeQueue.empty())
		frigg::infoLogger() << "Fix thread destructor!" << frigg::endLog;
}

Context &Thread::getContext() {
	return _context;
}

KernelUnsafePtr<Universe> Thread::getUniverse() {
	return _universe;
}
KernelUnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return _addressSpace;
}

void Thread::signalKill() {
	assert(_pendingSignal == kSigNone);
	_pendingSignal = kSigKill;

	if(_runState == kRunActive)
		return;

	frigg::panicLogger() << "Thread killed in inactive state" << frigg::endLog;
}

auto Thread::pendingSignal() -> Signal {
	return _pendingSignal;
}

void Thread::transitionToFault() {
	assert(!"Use Thread::faultCurrent() instead");
/*	assert(_runState == kRunActive);
	_runState = kRunFaulted;

	while(!_observeQueue.empty()) {
		frigg::SharedPtr<AsyncObserve> observe = _observeQueue.removeFront();
		AsyncOperation::complete(frigg::move(observe));
	}*/
}

void Thread::submitObserve(KernelSharedPtr<AsyncObserve> observe) {
	_observeQueue.addBack(frigg::move(observe));
}

void Thread::_blockLocked(frigg::LockGuard<Mutex> lock) {
	auto this_thread = getCurrentThread();
	assert(lock.protects(&this_thread->_mutex));
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunBlocked;

	assert(!intsAreEnabled());
	if(forkExecutor()) {
		runDetached([] (frigg::LockGuard<Mutex> lock) {
			// TODO: exit the current thread.
			lock.unlock();

			ScheduleGuard schedule_lock(scheduleLock.get());
			doSchedule(frigg::move(schedule_lock));
		}, frigg::move(lock));
	}
}

} // namespace thor

