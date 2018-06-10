
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

void Thread::deferCurrent() {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frigg::endLog;

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

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frigg::endLog;
	saveExecutor(&this_thread->_executor, image);

	runDetached([] (frigg::LockGuard<Mutex> lock) {
		lock.unlock();
		localScheduler()->reschedule();
	}, std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);

	frigg::infoLogger() << "interrupt " << (void *)this_thread.get()
			<< ", reason: " << (uint64_t)interrupt << frigg::endLog;
	assert(this_thread->_runState == kRunActive);
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frigg::endLog;
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	saveExecutor(&this_thread->_executor, image);

	Scheduler::suspend(this_thread.get());
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
			observe->trigger();
		}

		localScheduler()->reschedule();
	}, interrupt, this_thread.get(), std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frigg::guard(&this_thread->_mutex);

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frigg::endLog;
	saveExecutor(&this_thread->_executor, image);

	Scheduler::suspend(this_thread.get());
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
			observe->trigger();
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
		this_thread->_runState = kRunTerminated;
		++this_thread->_stateSeq;
	//	if(logRunStates)
			frigg::infoLogger() << "thor: " << (void *)this_thread.get()
					<< " was (asynchronously) killed" << frigg::endLog;
		saveExecutor(&this_thread->_executor, image);

		assert(!intsAreEnabled());
		Scheduler::suspend(this_thread.get());
		runDetached([] (Thread *thread, frigg::LockGuard<Mutex> lock) {
			ObserveQueue queue;
			queue.splice(queue.end(), thread->_observeQueue);

			lock.unlock();

			while(!queue.empty()) {
				auto observe = queue.pop_front();
				observe->error = Error::kErrThreadExited;
				observe->sequence = 0;
				observe->interrupt = kIntrNull;
				observe->trigger();
			}

			localScheduler()->reschedule();
		}, this_thread.get(), std::move(lock));
	}
	
	if(this_thread->_pendingSignal == kSigInterrupt) {
		this_thread->_runState = kRunInterrupted;
		this_thread->_lastInterrupt = kIntrRequested;
		++this_thread->_stateSeq;
		this_thread->_pendingSignal = kSigNone;
	//	if(logRunStates)
			frigg::infoLogger() << "thor: " << (void *)this_thread.get()
					<< " was (asynchronously) interrupted" << frigg::endLog;
		saveExecutor(&this_thread->_executor, image);

		assert(!intsAreEnabled());
		Scheduler::suspend(this_thread.get());
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
				observe->trigger();
			}

			localScheduler()->reschedule();
		}, this_thread.get(), std::move(lock));
	}
}

void Thread::unblockOther(frigg::UnsafePtr<Thread> thread) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&thread->_mutex);

	if(thread->_runState != kRunBlocked)
		return;

	thread->_runState = kRunDeferred;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)thread.get()
				<< " is deferred (via unblock)" << frigg::endLog;
	Scheduler::resume(thread.get());
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

	thread->_runState = kRunSuspended;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)thread.get()
				<< " is suspended (via resume)" << frigg::endLog;
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
	frigg::infoLogger() << "\e[31mthor: Shutting down thread\e[39m" << frigg::endLog;
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
		observe->trigger();
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
	observe->trigger();
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

	assert(_runState == kRunSuspended || _runState == kRunDeferred);
	_runState = kRunActive;
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

	lock.unlock();

	_userContext.migrate(getCpuData());
	_addressSpace->activate();
	getCpuData()->executorContext = &_executorContext;
	switchExecutor(self);
	restoreExecutor(&_executor);
}

void Thread::_blockLocked(frigg::LockGuard<Mutex> lock) {
	assert(!intsAreEnabled());
	auto this_thread = getCurrentThread();
	assert(lock.protects(&this_thread->_mutex));

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunBlocked;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is blocked" << frigg::endLog;

	forkExecutor([&] {
		Scheduler::suspend(this_thread.get());
		runDetached([] (frigg::LockGuard<Mutex> lock) {
			lock.unlock();
			localScheduler()->reschedule();
		}, frigg::move(lock));
	}, &this_thread->_executor);
}

void Thread::AssociatedWorkQueue::wakeup() {
	auto self = frg::container_of(this, &Thread::_associatedWorkQueue);
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&self->_mutex);

	if(self->_runState != kRunBlocked)
		return;

	self->_runState = kRunDeferred;
	if(logRunStates)
		frigg::infoLogger() << "thor: " << (void *)self
				<< " is deferred (via wq wakeup)" << frigg::endLog;
	Scheduler::resume(self);
}

} // namespace thor

