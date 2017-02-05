
#include "kernel.hpp"

namespace thor {

namespace {
	constexpr bool logTransitions = false;
	constexpr bool logRunStates = false;
}

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
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frigg::endLog;

	assert(!intsAreEnabled());
	if(forkExecutor(&this_thread->_executor)) {
		runDetached([] {
			globalScheduler().reschedule();
		});
	}
}

void Thread::deferCurrent(IrqImageAccessor image) {
	auto this_thread = getCurrentThread();
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frigg::endLog;
	saveExecutor(&this_thread->_executor, image);

	assert(!intsAreEnabled());
	runDetached([] {
		globalScheduler().reschedule();
	});
}

void Thread::blockCurrent(void *, void (*) (void *)) {
	assert(!"Use blockCurrentWhile() instead");
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image) {
	auto this_thread = getCurrentThread();
	frigg::infoLogger() << "interrupt " << (void *)this_thread.get()
			<< ", reason: " << (uint64_t)interrupt << frigg::endLog;
	assert(this_thread->_runState == kRunActive);
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is interrupted" << frigg::endLog;
	this_thread->_runState = kRunInterrupted;
	saveExecutor(&this_thread->_executor, image);

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
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is interrupted" << frigg::endLog;
	saveExecutor(&this_thread->_executor, image);

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
	if(logTransitions)
		frigg::infoLogger() << "thor: raiseSignals() in " << (void *)this_thread.get()
				<< frigg::endLog;
	assert(this_thread->_runState == kRunActive);
	
	if(this_thread->_pendingSignal == kSigStop) {
		this_thread->_runState = kRunInterrupted;
		if(logRunStates)
			frigg::infoLogger() << "thor: " << (void *)this_thread.get()
					<< " is interrupted" << frigg::endLog;
		saveExecutor(&this_thread->_executor, image);

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
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)thread.get()
				<< " is deferred (via unblock)" << frigg::endLog;
	globalScheduler().resume(thread.get());
}

void Thread::resumeOther(frigg::UnsafePtr<Thread> thread) {
	auto lock = frigg::guard(&thread->_mutex);
	assert(thread->_runState == kRunInterrupted);

	thread->_runState = kRunSuspended;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)thread.get()
				<< " is suspended (via resume)" << frigg::endLog;
	globalScheduler().resume(thread.get());
}

Thread::Thread(frigg::SharedPtr<Universe> universe,
		frigg::SharedPtr<AddressSpace> address_space, AbiParameters abi)
: flags(0), _runState(kRunInterrupted),
		_numTicks(0), _activationTick(0),
		_pendingSignal(kSigNone), _runCount(1),
		_executor{&_context, abi},
		_universe(frigg::move(universe)), _addressSpace(frigg::move(address_space)) {
//	frigg::infoLogger() << "[" << globalThreadId << "] New thread!" << frigg::endLog;
	auto stream = createStream();
	_superiorLane = frigg::move(stream.get<0>());
	_inferiorLane = frigg::move(stream.get<1>());
}

Thread::~Thread() {
	assert(_runState == kRunInterrupted);
	assert(_observeQueue.empty());
}

// This function has to initiate the thread's shutdown.
void Thread::destruct() {
	frigg::infoLogger() << "\e[31mShutting down thread\e[39m" << frigg::endLog;

	globalScheduler().detach(this);

	while(!_observeQueue.empty()) {
		auto observe = _observeQueue.pop_front();
		observe->error = Error::kErrThreadExited;
		observe->interrupt = kIntrNull;
		globalWorkQueue().post(observe);
	}
}

void Thread::cleanup() {
	frigg::destruct(*kernelAlloc, this);
}

void Thread::doSubmitObserve(ObserveBase *observe) {
	_observeQueue.push_back(observe);
}

UserContext &Thread::getContext() {
	return _context;
}

frigg::UnsafePtr<Universe> Thread::getUniverse() {
	return _universe;
}
frigg::UnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return _addressSpace;
}

void Thread::signalStop() {
	assert(_pendingSignal == kSigNone);
	_pendingSignal = kSigStop;
}

void Thread::invoke() {
	assert(_runState == kRunSuspended || _runState == kRunDeferred);
	_runState = kRunActive;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this << " is activated" << frigg::endLog;

	_context.migrate(getCpuData());
	_addressSpace->activate();
	switchExecutor(self);
	restoreExecutor(&_executor);
}

void Thread::_blockLocked(frigg::LockGuard<Mutex> lock) {
	auto this_thread = getCurrentThread();
	assert(lock.protects(&this_thread->_mutex));
	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunBlocked;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is blocked" << frigg::endLog;

	assert(!intsAreEnabled());
	if(forkExecutor(&this_thread->_executor)) {
		globalScheduler().suspend(this_thread.get());
		runDetached([] (frigg::LockGuard<Mutex> lock) {
			// TODO: exit the current thread.
			lock.unlock();

			globalScheduler().reschedule();
		}, frigg::move(lock));
	}
}

} // namespace thor

