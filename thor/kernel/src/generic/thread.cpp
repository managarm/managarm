
#include <stddef.h>
#include <string.h>

#include <frg/container_of.hpp>
#include "kernel.hpp"

namespace thor {

static std::atomic<uint64_t> globalThreadId;

namespace {
	constexpr bool logTransitions = false;
	constexpr bool logRunStates = false;
}

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::blockCurrent(ThreadBlocker *blocker) {
	auto this_thread = getCurrentThread();
	while(true) {
		// Run the WQ outside of the locks.
		this_thread->_associatedWorkQueue.run();

		StatelessIrqLock irq_lock;
		auto lock = frigg::guard(&this_thread->_mutex);

		// Those are the important tests; they are protected by the thread's mutex.
		if(blocker->_done)
			break;
		if(this_thread->_associatedWorkQueue.check())
			continue;
		
		if(logRunStates)
			frigg::infoLogger() << "thor: " << (void *)this_thread.get()
					<< " is blocked" << frigg::endLog;

		assert(this_thread->_runState == kRunActive);
		this_thread->_runState = kRunBlocked;
		Scheduler::suspendCurrent();
		this_thread->_uninvoke();

		forkExecutor([&] {
			runDetached([] (frigg::LockGuard<Mutex> lock) {
				lock.unlock();
				localScheduler()->reschedule();
			}, frigg::move(lock));
		}, &this_thread->_executor);
	}
}

void Thread::deferCurrent() {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frigg::endLog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;
	this_thread->_uninvoke();

	// TODO: We had forkExecutor() here. Did that serve any purpose?
	runDetached([] (frigg::LockGuard<Mutex> lock) {
		lock.unlock();
		localScheduler()->reschedule();
	}, std::move(lock));
}

void Thread::deferCurrent(IrqImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frigg::endLog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;
	saveExecutor(&this_thread->_executor, image);
	this_thread->_uninvoke();

	runDetached([] (frigg::LockGuard<Mutex> lock) {
		lock.unlock();
		localScheduler()->reschedule();
	}, std::move(lock));
}

void Thread::suspendCurrent(IrqImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is suspended" << frigg::endLog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunSuspended;
	saveExecutor(&this_thread->_executor, image);
	this_thread->_uninvoke();

	runDetached([] (frigg::LockGuard<Mutex> lock) {
		lock.unlock();
		localScheduler()->reschedule();
	}, std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);

	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frigg::endLog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	saveExecutor(&this_thread->_executor, image);
	Scheduler::suspendCurrent();
	this_thread->_uninvoke();

	runDetached([] (Interrupt interrupt, Thread *thread, frigg::LockGuard<Mutex> lock) {
		ObserveQueue queue;
		queue.splice(queue.end(), thread->_observeQueue);
		auto sequence = thread->_stateSeq;

		lock.unlock();

		while(!queue.empty()) {
			auto observe = queue.pop_front();
			observe->error = Error::kErrSuccess;
			observe->sequence = sequence;
			observe->interrupt = interrupt;
			WorkQueue::post(observe->triggered);
		}

		localScheduler()->reschedule();
	}, interrupt, this_thread.get(), std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frigg::endLog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	saveExecutor(&this_thread->_executor, image);
	Scheduler::suspendCurrent();
	this_thread->_uninvoke();

	runDetached([] (Interrupt interrupt, Thread *thread, frigg::LockGuard<Mutex> lock) {
		ObserveQueue queue;
		queue.splice(queue.end(), thread->_observeQueue);
		auto sequence = thread->_stateSeq;

		lock.unlock();

		while(!queue.empty()) {
			auto observe = queue.pop_front();
			observe->error = Error::kErrSuccess;
			observe->sequence = sequence;
			observe->interrupt = interrupt;
			WorkQueue::post(observe->triggered);
		}

		localScheduler()->reschedule();
	}, interrupt, this_thread.get(), std::move(lock));
}

void Thread::raiseSignals(SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);

	if(logTransitions)
		frigg::infoLogger() << "thor: raiseSignals() in " << (void *)this_thread.get()
				<< frigg::endLog;
	assert(this_thread->_runState == kRunActive);
	
	if(this_thread->_pendingKill) {
		if(logRunStates)
			frigg::infoLogger() << "thor: " << (void *)this_thread.get()
					<< " was (asynchronously) killed" << frigg::endLog;

		this_thread->_runState = kRunTerminated;
		++this_thread->_stateSeq;
		saveExecutor(&this_thread->_executor, image);
		Scheduler::suspendCurrent();
		this_thread->_uninvoke();

		runDetached([] (Thread *thread, frigg::LockGuard<Mutex> lock) {
			ObserveQueue queue;
			queue.splice(queue.end(), thread->_observeQueue);

			lock.unlock();

			while(!queue.empty()) {
				auto observe = queue.pop_front();
				observe->error = Error::kErrThreadExited;
				observe->sequence = 0;
				observe->interrupt = kIntrNull;
				WorkQueue::post(observe->triggered);
			}

			localScheduler()->reschedule();
		}, this_thread.get(), std::move(lock));
	}
	
	if(this_thread->_pendingSignal == kSigInterrupt) {
		if(logRunStates)
			frigg::infoLogger() << "thor: " << (void *)this_thread.get()
					<< " was (asynchronously) interrupted" << frigg::endLog;

		this_thread->_runState = kRunInterrupted;
		this_thread->_lastInterrupt = kIntrRequested;
		++this_thread->_stateSeq;
		this_thread->_pendingSignal = kSigNone;
		saveExecutor(&this_thread->_executor, image);
		Scheduler::suspendCurrent();
		this_thread->_uninvoke();

		runDetached([] (Thread *thread, frigg::LockGuard<Mutex> lock) {
			ObserveQueue queue;
			queue.splice(queue.end(), thread->_observeQueue);
			auto sequence = thread->_stateSeq;

			lock.unlock();

			while(!queue.empty()) {
				auto observe = queue.pop_front();
				observe->error = Error::kErrSuccess;
				observe->sequence = sequence;
				observe->interrupt = kIntrRequested;
				WorkQueue::post(observe->triggered);
			}

			localScheduler()->reschedule();
		}, this_thread.get(), std::move(lock));
	}
}

void Thread::unblockOther(ThreadBlocker *blocker) {
	auto thread = blocker->_thread;
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&thread->_mutex);

	assert(!blocker->_done);
	blocker->_done = true;

	if(thread->_runState != kRunBlocked)
		return;
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)thread
				<< " is deferred (via unblock)" << frigg::endLog;

	thread->_runState = kRunDeferred;
	Scheduler::resume(thread);
}

void Thread::killOther(frigg::UnsafePtr<Thread> thread) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&thread->_mutex);

	// TODO: Perform the kill immediately if possible.

	thread->_pendingKill = true;
}

void Thread::interruptOther(frigg::UnsafePtr<Thread> thread) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&thread->_mutex);

	// TODO: Perform the interrupt immediately if possible.

//	assert(thread->_pendingSignal == kSigNone);
	thread->_pendingSignal = kSigInterrupt;
}

void Thread::resumeOther(frigg::UnsafePtr<Thread> thread) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&thread->_mutex);

	assert(thread->_runState == kRunInterrupted);
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)thread.get()
				<< " is suspended (via resume)" << frigg::endLog;

	thread->_runState = kRunSuspended;
	Scheduler::resume(thread.get());
}

Thread::Thread(frigg::SharedPtr<Universe> universe,
		frigg::SharedPtr<AddressSpace> address_space, AbiParameters abi)
: flags(0), _runState(kRunInterrupted), _lastInterrupt{kIntrNull}, _stateSeq{1},
		_numTicks(0), _activationTick(0),
		_pendingKill{false}, _pendingSignal(kSigNone), _runCount(1),
		_executor{&_userContext, abi},
		_universe(frigg::move(universe)), _addressSpace(frigg::move(address_space)) {
	// TODO: Generate real UUIDs instead of ascending numbers.
	uint64_t id = globalThreadId.fetch_add(1, std::memory_order_relaxed) + 1;
	memset(_credentials, 0, 16);
	memcpy(_credentials + 8, &id, sizeof(uint64_t));

	_executorContext.associatedWorkQueue = &_associatedWorkQueue;

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
//	frigg::infoLogger() << "\e[31mthor: Shutting down thread\e[39m" << frigg::endLog;
	ObserveQueue queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		Scheduler::unassociate(this);
		
		queue.splice(queue.end(), _observeQueue);
	}

	while(!queue.empty()) {
		auto observe = queue.pop_front();
		observe->error = Error::kErrThreadExited;
		observe->sequence = 0;
		observe->interrupt = kIntrNull;
		WorkQueue::post(observe->triggered);
	}
}

void Thread::cleanup() {
	frigg::destruct(*kernelAlloc, this);
}

void Thread::doSubmitObserve(uint64_t in_seq, ObserveBase *observe) {
	RunState state;
	Interrupt interrupt;
	uint64_t sequence;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&_mutex);

		assert(in_seq <= _stateSeq);
		if(in_seq == _stateSeq && _runState != kRunTerminated) {
			_observeQueue.push_back(observe);
			return;
		}else{
			state = _runState;
			interrupt = _lastInterrupt;
			sequence = _stateSeq;
		}
	}

	switch(state) {
	case kRunInterrupted:
		observe->error = Error::kErrSuccess;
		observe->sequence = sequence;
		observe->interrupt = interrupt;
		break;
	case kRunTerminated:
		observe->error = Error::kErrThreadExited;
		observe->sequence = 0;
		observe->interrupt = kIntrNull;
		break;
	default:
		frigg::panicLogger() << "thor: Unexpected RunState" << frigg::endLog;
	}
	WorkQueue::post(observe->triggered);
}

UserContext &Thread::getContext() {
	return _userContext;
}

frigg::UnsafePtr<Universe> Thread::getUniverse() {
	return _universe;
}
frigg::UnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return _addressSpace;
}

void Thread::invoke() {
	assert(!intsAreEnabled());
	auto lock = frigg::guard(&_mutex);
	
	if(logRunStates)
		frigg::infoLogger() << "thor: "
				<< " " << _credentials[0] << " " << _credentials[1]
				<< " " << _credentials[2] << " " << _credentials[3]
				<< " " << _credentials[4] << " " << _credentials[5]
				<< " " << _credentials[6] << " " << _credentials[7]
				<< " " << _credentials[8] << " " << _credentials[9]
				<< " " << _credentials[10] << " " << _credentials[11]
				<< " " << _credentials[12] << " " << _credentials[13]
				<< " " << _credentials[14] << " " << _credentials[15]
				<< " is activated" << frigg::endLog;

	// If there is work to do, return to the WorkQueue and not to user space.
	if(_runState == kRunSuspended && _associatedWorkQueue.check())
		workOnExecutor(&_executor);

	assert(_runState == kRunSuspended || _runState == kRunDeferred);
	_runState = kRunActive;

	lock.unlock();

	_userContext.migrate(getCpuData());
	_addressSpace->activate();
	getCpuData()->executorContext = &_executorContext;
	switchExecutor(self);
	restoreExecutor(&_executor);
}

void Thread::_uninvoke() {
	UserContext::deactivate();
}

void Thread::AssociatedWorkQueue::wakeup() {
	auto self = frg::container_of(this, &Thread::_associatedWorkQueue);
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&self->_mutex);

	if(self->_runState != kRunBlocked)
		return;
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)self
				<< " is deferred (via wq wakeup)" << frigg::endLog;

	self->_runState = kRunDeferred;
	Scheduler::resume(self);
}

void ThreadBlocker::setup() {
	_thread = getCurrentThread().get();
	_done = false;
}

} // namespace thor

