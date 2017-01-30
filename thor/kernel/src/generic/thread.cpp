
#include "kernel.hpp"

namespace thor {

void Thread::ObserveBase::run() {
	trigger(error, interrupt);
}

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::deferCurrent() {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;

	assert(!intsAreEnabled());
	if(forkExecutor()) {
		runDetached([] {
			globalScheduler().reschedule();
		});
	}
}

void Thread::blockCurrent(void *, void (*) (void *)) {
	assert(!"Use blockCurrentWhile() instead");
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image) {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	saveExecutor(image);

	while(!this_thread->_observeQueue.empty()) {
		auto observe = this_thread->_observeQueue.pop_front();
		observe->error = Error::kErrSuccess;
		observe->interrupt = interrupt;
		observe->trigger(Error::kErrSuccess, interrupt);
		globalWorkQueue().post(observe);
	}

	assert(!intsAreEnabled());
	globalScheduler().suspend(this_thread.get());
	runDetached([] {
		globalScheduler().reschedule();
	});
}

void Thread::interruptCurrent(Interrupt interrupt, SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	saveExecutor(image);

	while(!this_thread->_observeQueue.empty()) {
		auto observe = this_thread->_observeQueue.pop_front();
		observe->error = Error::kErrSuccess;
		observe->interrupt = interrupt;
		globalWorkQueue().post(observe);
	}

	assert(!intsAreEnabled());
	globalScheduler().suspend(this_thread.get());
	runDetached([] {
		globalScheduler().reschedule();
	});
}

void Thread::raiseSignals(SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	
	if(this_thread->_pendingSignal == kSigStop) {
		this_thread->_runState = kRunInterrupted;
		saveExecutor(image);

		while(!this_thread->_observeQueue.empty()) {
			auto observe = this_thread->_observeQueue.pop_front();
			observe->error = Error::kErrSuccess;
			observe->interrupt = kIntrStop;
			globalWorkQueue().post(observe);
		}

		assert(!intsAreEnabled());
		globalScheduler().suspend(this_thread.get());
		runDetached([] {
			globalScheduler().reschedule();
		});
	}
}

void Thread::activateOther(frigg::UnsafePtr<Thread> other_thread) {
	assert(!"TODO: Remove this");
}

void Thread::unblockOther(frigg::UnsafePtr<Thread> thread) {
	auto lock = frigg::guard(&thread->_mutex);
	if(thread->_runState != kRunBlocked)
		return;

	thread->_runState = kRunDeferred;
	globalScheduler().resume(thread.get());
}

void Thread::resumeOther(frigg::UnsafePtr<Thread> thread) {
	auto lock = frigg::guard(&thread->_mutex);
	assert(thread->_runState == kRunInterrupted);

	thread->_runState = kRunSuspended;
	globalScheduler().resume(thread.get());
}

Thread::Thread(KernelSharedPtr<Universe> universe,
		KernelSharedPtr<AddressSpace> address_space)
: flags(0), _runState(kRunInterrupted),
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
	assert(!"Graciously shut down threads");
	if(!_observeQueue.empty())
		frigg::infoLogger() << "\e[35mFix thread destructor!\e[39m" << frigg::endLog;
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

void Thread::signalStop() {
	assert(_pendingSignal == kSigNone);
	_pendingSignal = kSigStop;
}

void Thread::invoke() {
	assert(_runState == kRunSuspended || _runState == kRunDeferred);
	_runState = kRunActive;

	_addressSpace->activate();
	switchContext(&_context);
	switchExecutor(self);
	restoreExecutor();
}

void Thread::_blockLocked(frigg::LockGuard<Mutex> lock) {
	auto this_thread = getCurrentThread();
	assert(lock.protects(&this_thread->_mutex));
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunBlocked;

	assert(!intsAreEnabled());
	if(forkExecutor()) {
		globalScheduler().suspend(this_thread.get());
		runDetached([] (frigg::LockGuard<Mutex> lock) {
			// TODO: exit the current thread.
			lock.unlock();

			globalScheduler().reschedule();
		}, frigg::move(lock));
	}
}

} // namespace thor

