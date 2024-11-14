#include <stddef.h>
#include <string.h>

#include <frg/container_of.hpp>

#include <thor-internal/credentials.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

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
	auto lock = frg::guard(&this_thread->_mutex);

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
	localScheduler()->forceReschedule();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler()->commitReschedule();
		}, getCpuData()->detachedStack.base(), &this_thread->_executor, std::move(lock));
	}, &this_thread->_executor);
}

void Thread::blockCurrent() {
	auto thisThread = getCurrentThread();

	// Optimistically clear the unblock latch before entering the mutex.
	// We need acquire semantics to synchronize with unblockOther().
	if(thisThread->_unblockLatch.exchange(false, std::memory_order_acquire))
		return;

	StatelessIrqLock irqLock;
	auto lock = frg::guard(&thisThread->_mutex);

	// We do not need any memory barrier here: no matter how our aquisition of _mutex
	// is ordered to the aquisition in unblockOther(), we are still correct.
	if(thisThread->_unblockLatch.load(std::memory_order_relaxed))
		return;

	if(logRunStates)
		infoLogger() << "thor: " << (void *)thisThread.get()
				<< " is blocked" << frg::endlog;

	assert(thisThread->_runState == kRunActive);
	thisThread->_runState = kRunBlocked;
	getCpuData()->scheduler.update();
	Scheduler::suspendCurrent();
	getCpuData()->scheduler.forceReschedule();
	thisThread->_uninvoke();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler()->commitReschedule();
		}, getCpuData()->detachedStack.base(), &thisThread->_executor, std::move(lock));
	}, &thisThread->_executor);
}

void Thread::deferCurrent() {
	auto thisThread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&thisThread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)thisThread.get()
				<< " is deferred" << frg::endlog;

	assert(thisThread->_runState == kRunActive);
	thisThread->_runState = kRunDeferred;
	getCpuData()->scheduler.update();
	getCpuData()->scheduler.forceReschedule();
	thisThread->_uninvoke();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler()->commitReschedule();
		}, getCpuData()->detachedStack.base(), &thisThread->_executor, std::move(lock));
	}, &thisThread->_executor);
}

void Thread::deferCurrent(IrqImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is deferred" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunDeferred;
	saveExecutor(&this_thread->_executor, image);
	getCpuData()->scheduler.update();
	getCpuData()->scheduler.forceReschedule();
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		lock.unlock();
		localScheduler()->commitReschedule();
	}, getCpuData()->detachedStack.base(), image, std::move(lock));
}

void Thread::suspendCurrent(IrqImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is suspended" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunSuspended;
	saveExecutor(&this_thread->_executor, image);
	getCpuData()->scheduler.update();
	getCpuData()->scheduler.forceReschedule();
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		lock.unlock();
		localScheduler()->commitReschedule();
	}, getCpuData()->detachedStack.base(), image, std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);

	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	saveExecutor(&this_thread->_executor, image);
	getCpuData()->scheduler.update();
	Scheduler::suspendCurrent();
	getCpuData()->scheduler.forceReschedule();
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, FaultImageAccessor image,
			Interrupt interrupt, Thread *thread, frg::unique_lock<Mutex> lock) {
		ObserveQueue queue;
		queue.splice(queue.end(), thread->_observeQueue);
		auto sequence = thread->_stateSeq;

		scrubStack(image, cont);
		lock.unlock();

		while(!queue.empty()) {
			auto node = queue.pop_front();
			async::execution::set_value(node->receiver,
					frg::make_tuple(Error::success, sequence, interrupt));
		}

		localScheduler()->commitReschedule();
	}, getCpuData()->detachedStack.base(), image, interrupt, this_thread.get(), std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	saveExecutor(&this_thread->_executor, image);
	getCpuData()->scheduler.update();
	Scheduler::suspendCurrent();
	getCpuData()->scheduler.forceReschedule();
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, SyscallImageAccessor image,
			Interrupt interrupt, Thread *thread, frg::unique_lock<Mutex> lock) {
		ObserveQueue queue;
		queue.splice(queue.end(), thread->_observeQueue);
		auto sequence = thread->_stateSeq;

		scrubStack(image, cont);
		lock.unlock();

		while(!queue.empty()) {
			auto node = queue.pop_front();
			async::execution::set_value(node->receiver,
					frg::make_tuple(Error::success, sequence, interrupt));
		}

		localScheduler()->commitReschedule();
	}, getCpuData()->detachedStack.base(), image, interrupt, this_thread.get(), std::move(lock));
}

void Thread::raiseSignals(SyscallImageAccessor image) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);

	if(logTransitions)
		infoLogger() << "thor: raiseSignals() in " << (void *)this_thread.get()
				<< frg::endlog;
	assert(this_thread->_runState == kRunActive);
	
	if(this_thread->_pendingKill) {
		if(logRunStates)
			infoLogger() << "thor: " << (void *)this_thread.get()
					<< " was (asynchronously) killed" << frg::endlog;

		this_thread->_runState = kRunTerminated;
		++this_thread->_stateSeq;
		saveExecutor(&this_thread->_executor, image); // FIXME: Why do we save the state here?
		getCpuData()->scheduler.update();
		Scheduler::suspendCurrent();
		Scheduler::unassociate(this_thread.get());
		getCpuData()->scheduler.forceReschedule();
		this_thread->_uninvoke();

		runOnStack([] (Continuation cont, SyscallImageAccessor image,
				Thread *thread, frg::unique_lock<Mutex> lock) {
			ObserveQueue queue;
			queue.splice(queue.end(), thread->_observeQueue);

			scrubStack(image, cont);
			lock.unlock();

			while(!queue.empty()) {
				auto node = queue.pop_front();
				async::execution::set_value(node->receiver,
						frg::make_tuple(Error::threadExited, 0, kIntrNull));
			}

			localScheduler()->commitReschedule();
		}, getCpuData()->detachedStack.base(), image, this_thread.get(), std::move(lock));
	}else if(this_thread->_pendingSignal == kSigInterrupt) {
		if(logRunStates)
			infoLogger() << "thor: " << (void *)this_thread.get()
					<< " was (asynchronously) interrupted" << frg::endlog;

		this_thread->_runState = kRunInterrupted;
		this_thread->_lastInterrupt = kIntrRequested;
		++this_thread->_stateSeq;
		this_thread->_pendingSignal = kSigNone;
		saveExecutor(&this_thread->_executor, image);
		getCpuData()->scheduler.update();
		Scheduler::suspendCurrent();
		getCpuData()->scheduler.forceReschedule();
		this_thread->_uninvoke();

		runOnStack([] (Continuation cont, SyscallImageAccessor image,
				Thread *thread, frg::unique_lock<Mutex> lock) {
			ObserveQueue queue;
			queue.splice(queue.end(), thread->_observeQueue);
			auto sequence = thread->_stateSeq;

			scrubStack(image, cont);
			lock.unlock();

			while(!queue.empty()) {
				auto node = queue.pop_front();
				async::execution::set_value(node->receiver,
						frg::make_tuple(Error::success, sequence, kIntrRequested));
			}

			localScheduler()->commitReschedule();
		}, getCpuData()->detachedStack.base(), image, this_thread.get(), std::move(lock));
	}
}

void Thread::unblockOther(smarter::borrowed_ptr<Thread> thread) {
	// Release semantics ensure that we synchronize with the thread when it flips the flag
	// back to false. Acquire semantics are needed to synchronize with other threads
	// that already set the flag to true in the meantime.
	auto ul = thread->_unblockLatch.exchange(true, std::memory_order_acq_rel);
	if(ul)
		return;

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&thread->_mutex);

	if(thread->_runState != kRunBlocked)
		return;
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)thread.get()
				<< " is deferred (via unblock)" << frg::endlog;

	thread->_runState = kRunDeferred;
	Scheduler::resume(thread.get());
}

void Thread::killOther(smarter::borrowed_ptr<Thread> thread) {
	thread->_kill();
}

void Thread::interruptOther(smarter::borrowed_ptr<Thread> thread) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&thread->_mutex);

	// TODO: Perform the interrupt immediately if possible.

//	assert(thread->_pendingSignal == kSigNone);
	thread->_pendingSignal = kSigInterrupt;
}

Error Thread::resumeOther(smarter::borrowed_ptr<Thread> thread) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&thread->_mutex);

	if(thread->_runState == kRunTerminated)
		return Error::threadExited;
	if(thread->_runState != kRunInterrupted)
		return Error::illegalState;
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)thread.get()
				<< " is suspended (via resume)" << frg::endlog;

	thread->_runState = kRunSuspended;
	Scheduler::resume(thread.get());
	return Error::success;
}

Thread::Thread(smarter::shared_ptr<Universe> universe,
		smarter::shared_ptr<AddressSpace, BindableHandle> address_space, AbiParameters abi)
: flags{0}, _mainWorkQueue{this}, _pagingWorkQueue{this},
		_runState{kRunInterrupted}, _lastInterrupt{kIntrNull}, _stateSeq{1},
		_pendingKill{false}, _pendingSignal{kSigNone}, _runCount{1},
		_executor{&_userContext, abi},
		_universe{std::move(universe)}, _addressSpace{std::move(address_space)},
		_affinityMask{*kernelAlloc} {
}

Thread::~Thread() {
	assert(_runState == kRunTerminated);
	assert(_observeQueue.empty());
}

// This function has to initiate the thread's shutdown.
void Thread::dispose(ActiveHandle) {
	if(logCleanup)
		urgentLogger() << "thor: Killing thread due to destruction" << frg::endlog;
	_kill();
	_mainWorkQueue.selfPtr = {};
	_pagingWorkQueue.selfPtr = {};
}

void Thread::observe_(uint64_t inSeq, ObserveNode *node) {
	RunState state;
	Interrupt interrupt;
	uint64_t sequence;
	{
		auto irqLock = frg::guard(&irqMutex());
		auto lock = frg::guard(&_mutex);

		assert(inSeq <= _stateSeq);
		if(inSeq == _stateSeq && _runState != kRunTerminated) {
			_observeQueue.push_back(node);
			return;
		}else{
			state = _runState;
			interrupt = _lastInterrupt;
			sequence = _stateSeq;
		}
	}

	switch(state) {
	case kRunInterrupted:
		async::execution::set_value(node->receiver,
				frg::make_tuple(Error::success, sequence, interrupt));
		break;
	case kRunTerminated:
		async::execution::set_value(node->receiver,
				frg::make_tuple(Error::threadExited, 0, kIntrNull));
		break;
	default:
		panicLogger() << "thor: Unexpected RunState" << frg::endlog;
	}
}

UserContext &Thread::getContext() {
	return _userContext;
}

smarter::borrowed_ptr<Universe> Thread::getUniverse() {
	return _universe;
}
smarter::borrowed_ptr<AddressSpace, BindableHandle> Thread::getAddressSpace() {
	return _addressSpace;
}

void Thread::invoke() {
	assert(!intsAreEnabled());
	auto lock = frg::guard(&_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: "
				<< " " << _credentials[0] << " " << _credentials[1]
				<< " " << _credentials[2] << " " << _credentials[3]
				<< " " << _credentials[4] << " " << _credentials[5]
				<< " " << _credentials[6] << " " << _credentials[7]
				<< " " << _credentials[8] << " " << _credentials[9]
				<< " " << _credentials[10] << " " << _credentials[11]
				<< " " << _credentials[12] << " " << _credentials[13]
				<< " " << _credentials[14] << " " << _credentials[15]
				<< " is activated" << frg::endlog;

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

void Thread::handlePreemption(IrqImageAccessor image) {
	assert(!intsAreEnabled());
	assert(getCurrentThread().get() == this);

	localScheduler()->update();
	if(localScheduler()->maybeReschedule()) {
		auto lock = frg::guard(&_mutex);

		if(logRunStates)
			infoLogger() << "thor: " << (void *)this << " is deferred" << frg::endlog;

		assert(_runState == kRunActive);
		if(image.inManipulableDomain()) {
			_runState = kRunSuspended;
		}else{
			_runState = kRunDeferred;
		}
		saveExecutor(&_executor, image);
		_uninvoke();

		runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<Mutex> lock) {
			scrubStack(image, cont);
			lock.unlock();
			localScheduler()->commitReschedule();
		}, getCpuData()->detachedStack.base(), image, std::move(lock));
	}else{
		localScheduler()->renewSchedule();
	}
}

void Thread::_uninvoke() {
	UserContext::deactivate();
}

void Thread::_kill() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

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
			auto node = queue.pop_front();
			async::execution::set_value(node->receiver,
					frg::make_tuple(Error::threadExited, 0, kIntrNull));
		}
	}else{
		// TODO: Wake up blocked threads.
		_pendingKill = true;
	}
}

void Thread::AssociatedWorkQueue::wakeup() {
	unblockOther(_thread->self);
}

} // namespace thor
