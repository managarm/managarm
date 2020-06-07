
#include <stddef.h>
#include <string.h>

#include <frg/container_of.hpp>
#include "kernel.hpp"

#include <generic/core.hpp>

namespace thor {

static std::atomic<uint64_t> globalThreadId;

namespace {
	constexpr bool logTransitions = false;
	constexpr bool logRunStates = false;
	constexpr bool logCleanup = false;
}

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::migrateCurrent() {
	auto this_thread = getCurrentThread().get();

	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);

	assert(this_thread->_runState == kRunActive);
	getCpuData()->scheduler.update();
	Scheduler::suspendCurrent();
	this_thread->_runState = kRunDeferred;
	this_thread->_uninvoke();

	Scheduler::unassociate(this_thread);

	size_t n = -1;
	for (size_t i = 0; i < getCpuCount(); i++) {
		bool bit = 0;
		if ((i + 7) / 8 < this_thread->_affinityMask.size())
			bit = this_thread->_affinityMask[(i + 7) / 8] & (1 << (i % 8));

		if (bit) {
			n = i;
			break;
		}
	}

	auto new_scheduler = &getCpuData(n)->scheduler;

	Scheduler::associate(this_thread, new_scheduler);
	Scheduler::resume(this_thread);
	localScheduler()->reschedule();

	forkExecutor([&] {
		runDetached([] (frigg::LockGuard<Mutex> lock) {
			lock.unlock();
			localScheduler()->commitReschedule();
		}, frigg::move(lock));
	}, &this_thread->_executor);
}

void Thread::blockCurrent(ThreadBlocker *blocker) {
	auto this_thread = getCurrentThread();
	while(true) {
		// Run the WQ outside of the locks.
		WorkQueue::localQueue()->run();

		StatelessIrqLock irq_lock;
		auto lock = frigg::guard(&this_thread->_mutex);

		// Those are the important tests; they are protected by the thread's mutex.
		if(blocker->_done)
			break;
		if(WorkQueue::localQueue()->check())
			continue;
		
		if(logRunStates)
			frigg::infoLogger() << "thor: " << (void *)this_thread.get()
					<< " is blocked" << frigg::endLog;

		assert(this_thread->_runState == kRunActive);
		this_thread->_runState = kRunBlocked;
		getCpuData()->scheduler.update();
		Scheduler::suspendCurrent();
		getCpuData()->scheduler.reschedule();
		this_thread->_uninvoke();

		forkExecutor([&] {
			runDetached([] (frigg::LockGuard<Mutex> lock) {
				lock.unlock();
				localScheduler()->commitReschedule();
			}, frigg::move(lock));
		}, &this_thread->_executor);
	}
}

// FIXME: This function does not save the state! It needs to be given a ImageAccessor parameter!
void Thread::deferCurrent() {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frigg::endLog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;
	getCpuData()->scheduler.update();
	getCpuData()->scheduler.reschedule();
	this_thread->_uninvoke();

	runDetached([] (frigg::LockGuard<Mutex> lock) {
		lock.unlock();
		localScheduler()->commitReschedule();
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
	getCpuData()->scheduler.update();
	getCpuData()->scheduler.reschedule();
	this_thread->_uninvoke();

	runDetached([] (frigg::LockGuard<Mutex> lock) {
		lock.unlock();
		localScheduler()->commitReschedule();
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
	getCpuData()->scheduler.update();
	getCpuData()->scheduler.reschedule();
	this_thread->_uninvoke();

	runDetached([] (frigg::LockGuard<Mutex> lock) {
		lock.unlock();
		localScheduler()->commitReschedule();
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
	getCpuData()->scheduler.update();
	Scheduler::suspendCurrent();
	getCpuData()->scheduler.reschedule();
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

		localScheduler()->commitReschedule();
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
	getCpuData()->scheduler.update();
	Scheduler::suspendCurrent();
	getCpuData()->scheduler.reschedule();
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

		localScheduler()->commitReschedule();
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
		saveExecutor(&this_thread->_executor, image); // FIXME: Why do we save the state here?
		getCpuData()->scheduler.update();
		Scheduler::suspendCurrent();
		Scheduler::unassociate(this_thread.get());
		getCpuData()->scheduler.reschedule();
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

			localScheduler()->commitReschedule();
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
		getCpuData()->scheduler.update();
		Scheduler::suspendCurrent();
		getCpuData()->scheduler.reschedule();
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

			localScheduler()->commitReschedule();
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
	thread->_kill();
}

void Thread::interruptOther(frigg::UnsafePtr<Thread> thread) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&thread->_mutex);

	// TODO: Perform the interrupt immediately if possible.

//	assert(thread->_pendingSignal == kSigNone);
	thread->_pendingSignal = kSigInterrupt;
}

Error Thread::resumeOther(frigg::UnsafePtr<Thread> thread) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&thread->_mutex);

	if(thread->_runState == kRunTerminated)
		return kErrThreadExited;
	if(thread->_runState != kRunInterrupted)
		return kErrIllegalState;
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)thread.get()
				<< " is suspended (via resume)" << frigg::endLog;

	thread->_runState = kRunSuspended;
	Scheduler::resume(thread.get());
	return kErrSuccess;
}

Thread::Thread(frigg::SharedPtr<Universe> universe,
		smarter::shared_ptr<AddressSpace, BindableHandle> address_space, AbiParameters abi)
: flags{0}, _mainWorkQueue{this}, _pagingWorkQueue{this},
		_runState{kRunInterrupted}, _lastInterrupt{kIntrNull}, _stateSeq{1},
		_numTicks{0}, _activationTick{0},
		_pendingKill{false}, _pendingSignal{kSigNone}, _runCount{1},
		_executor{&_userContext, abi},
		_universe{frigg::move(universe)}, _addressSpace{frigg::move(address_space)},
		_affinityMask{*kernelAlloc} {
	// TODO: Generate real UUIDs instead of ascending numbers.
	uint64_t id = globalThreadId.fetch_add(1, std::memory_order_relaxed) + 1;
	memset(_credentials, 0, 16);
	memcpy(_credentials + 8, &id, sizeof(uint64_t));

	_executorContext.associatedWorkQueue = &_mainWorkQueue;

	auto stream = createStream();
	_superiorLane = frigg::move(stream.get<0>());
	_inferiorLane = frigg::move(stream.get<1>());
}

Thread::~Thread() {
	assert(_runState == kRunTerminated);
	assert(_observeQueue.empty());
}

// This function has to initiate the thread's shutdown.
void Thread::destruct() {
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: Killing thread due to destruction\e[39m"
				<< frigg::endLog;
	_kill();
}

void Thread::cleanup() {
	// TODO: Audit code to make sure this is called late enough (i.e. after termination completes).
	// TODO: Make sure that this is called!
	if(logCleanup)
		frigg::infoLogger() << "\e[31mthor: Thread is destructed\e[39m" << frigg::endLog;
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
smarter::borrowed_ptr<AddressSpace, BindableHandle> Thread::getAddressSpace() {
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
	if(_runState == kRunSuspended && _mainWorkQueue.check())
		workOnExecutor(&_executor);

	assert(_runState == kRunSuspended || _runState == kRunDeferred);
	_runState = kRunActive;

	lock.unlock();

	_userContext.migrate(getCpuData());
	AddressSpace::activate(_addressSpace);
	getCpuData()->executorContext = &_executorContext;
	switchExecutor(self);
	restoreExecutor(&_executor);
}

void Thread::_uninvoke() {
	UserContext::deactivate();
}

void Thread::_kill() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	if(_runState == kRunTerminated)
		return;

	if(_runState == kRunSuspended || _runState == kRunInterrupted) {
		_runState = kRunTerminated;
		++_stateSeq;
		Scheduler::unassociate(this);

		ObserveQueue queue;
		queue.splice(queue.end(), _observeQueue);

		lock.unlock();

		while(!queue.empty()) {
			auto observe = queue.pop_front();
			observe->error = Error::kErrThreadExited;
			observe->sequence = 0;
			observe->interrupt = kIntrNull;
			WorkQueue::post(observe->triggered);
		}
	}else{
		// TODO: Wake up blocked threads.
		_pendingKill = true;
	}
}

void Thread::AssociatedWorkQueue::wakeup() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_thread->_mutex);

	if(_thread->_runState != kRunBlocked)
		return;
	
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)_thread
				<< " is deferred (via wq wakeup)" << frigg::endLog;

	_thread->_runState = kRunDeferred;
	Scheduler::resume(_thread);
}

void ThreadBlocker::setup() {
	_thread = getCurrentThread().get();
	_done = false;
}

} // namespace thor

