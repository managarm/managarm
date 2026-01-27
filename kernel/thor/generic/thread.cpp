#include <stddef.h>
#include <string.h>

#include <frg/container_of.hpp>

#include <thor-internal/credentials.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

namespace {
	constexpr bool logTransitions = false;
	constexpr bool logRunStates = false;
	constexpr bool logMigration = false;
	constexpr bool logCleanup = false;
}

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

void Thread::migrateCurrent() {
	auto this_thread = getCurrentThread().get();
	auto maskSize = LbControlBlock::affinityMaskSize();

	frg::vector<uint8_t, KernelAlloc> mask{*kernelAlloc};
	mask.resize(maskSize);
	this_thread->_lbCb->getAffinityMask({mask.data(), maskSize});

	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);

	assert(this_thread->_runState == kRunActive);
	localScheduler.get().update();
	Scheduler::suspendCurrent();
	this_thread->_updateRunTime();
	this_thread->_runState = kRunDeferred;
	this_thread->_uninvoke();

	Scheduler::unassociate(this_thread);

	size_t n = -1;
	for (size_t i = 0; i < getCpuCount(); i++) {
		bool bit = 0;
		if ((i + 7) / 8 < mask.size())
			bit = mask[(i + 7) / 8] & (1 << (i % 8));

		if (bit) {
			n = i;
			break;
		}
	}
	// Affinity masks are guaranteed to not be all zeros.
	assert(n != static_cast<size_t>(-1));

	auto *new_scheduler = &localScheduler.getFor(n);

	Scheduler::associate(this_thread, new_scheduler);
	Scheduler::resume(this_thread);
	localScheduler.get().forceReschedule();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), &this_thread->_executor, std::move(lock));
	}, &this_thread->_executor);
}

bool Thread::blockCurrent(bool interruptible) {
	auto thisThread = getCurrentThread();

	// Optimistically clear the unblock latch before entering the mutex.
	// We need acquire semantics to synchronize with unblockOther().
	if(thisThread->_unblockLatch.exchange(false, std::memory_order_acquire))
		return true;

	StatelessIrqLock irqLock;
	auto lock = frg::guard(&thisThread->_mutex);

	// We do not need any memory barrier here: no matter how our aquisition of _mutex
	// is ordered to the aquisition in unblockOther(), we are still correct.
	if(thisThread->_unblockLatch.load(std::memory_order_relaxed))
		return true;

	if(logRunStates)
		infoLogger() << "thor: " << (void *)thisThread.get()
				<< " is blocked" << frg::endlog;

	assert(thisThread->_runState == kRunActive);
	thisThread->_updateRunTime();
	thisThread->_runState = interruptible ? kRunInterruptableBlocked : kRunBlocked;
	localScheduler.get().update();
	Scheduler::suspendCurrent();
	localScheduler.get().forceReschedule();
	thisThread->_uninvoke();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), &thisThread->_executor, std::move(lock));
	}, &thisThread->_executor);

	// Check if we've been interrupted
	if (interruptible && thisThread->_pendingSignal == kSigInterrupt)
		return false;
	return true;
}

void Thread::deferCurrent() {
	auto thisThread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&thisThread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)thisThread.get()
				<< " is deferred" << frg::endlog;

	assert(thisThread->_runState == kRunActive);
	thisThread->_updateRunTime();
	thisThread->_runState = kRunDeferred;
	localScheduler.get().update();
	localScheduler.get().forceReschedule();
	thisThread->_uninvoke();

	forkExecutor([&] {
		runOnStack([] (Continuation cont, Executor *executor, frg::unique_lock<Mutex> lock) {
			scrubStack(executor, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
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
	this_thread->_updateRunTime();
	this_thread->_runState = kRunDeferred;
	saveExecutor(&this_thread->_executor, image);
	localScheduler.get().update();
	localScheduler.get().forceReschedule();
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		lock.unlock();
		localScheduler.get().commitReschedule();
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
	this_thread->_updateRunTime();
	this_thread->_runState = kRunSuspended;
	saveExecutor(&this_thread->_executor, image);
	localScheduler.get().update();
	localScheduler.get().forceReschedule();
	this_thread->_uninvoke();

	runOnStack([] (Continuation cont, IrqImageAccessor image, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		lock.unlock();
		localScheduler.get().commitReschedule();
	}, getCpuData()->detachedStack.base(), image, std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, FaultImageAccessor image, InterruptInfo info) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);

	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_updateRunTime();
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	saveExecutor(&this_thread->_executor, image);
	this_thread->_uninvoke();

	localScheduler.get().updateState();
	Scheduler::suspendCurrent();

	this_thread->interruptInfo = info;

	runOnStack([] (Continuation cont, FaultImageAccessor image,
			Interrupt interrupt, Thread *thread, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		auto *scheduler = &localScheduler.get();

		ObserveQueue queue;
		queue.splice(queue.end(), thread->_observeQueue);
		auto sequence = thread->_stateSeq;

		lock.unlock();

		// Run observer callbacks before re-scheduling (as callbacks may unblock threads).
		while(!queue.empty()) {
			auto node = queue.pop_front();
			async::execution::set_value(node->receiver,
					frg::make_tuple(Error::success, sequence, interrupt));
		}

		scheduler->updateQueue();
		scheduler->forceReschedule();
		scheduler->commitReschedule();
	}, getCpuData()->detachedStack.base(), image, interrupt, this_thread.get(), std::move(lock));
}

void Thread::interruptCurrent(Interrupt interrupt, SyscallImageAccessor image, InterruptInfo info) {
	auto this_thread = getCurrentThread();
	StatelessIrqLock irq_lock;
	auto lock = frg::guard(&this_thread->_mutex);
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)this_thread.get()
				<< " is (synchronously) interrupted" << frg::endlog;

	assert(this_thread->_runState == kRunActive);
	this_thread->_updateRunTime();
	this_thread->_runState = kRunInterrupted;
	this_thread->_lastInterrupt = interrupt;
	++this_thread->_stateSeq;
	saveExecutor(&this_thread->_executor, image);
	this_thread->_uninvoke();
	this_thread->interruptInfo = info;

	localScheduler.get().updateState();
	Scheduler::suspendCurrent();

	runOnStack([] (Continuation cont, SyscallImageAccessor image,
			Interrupt interrupt, Thread *thread, frg::unique_lock<Mutex> lock) {
		scrubStack(image, cont);
		auto *scheduler = &localScheduler.get();

		ObserveQueue queue;
		queue.splice(queue.end(), thread->_observeQueue);
		auto sequence = thread->_stateSeq;

		lock.unlock();

		// Run observer callbacks before re-scheduling (as callbacks may unblock threads).
		while(!queue.empty()) {
			auto node = queue.pop_front();
			async::execution::set_value(node->receiver,
					frg::make_tuple(Error::success, sequence, interrupt));
		}

		scheduler->updateQueue();
		scheduler->forceReschedule();
		scheduler->commitReschedule();
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

		this_thread->_updateRunTime();
		this_thread->_runState = kRunTerminated;
		++this_thread->_stateSeq;
		saveExecutor(&this_thread->_executor, image); // FIXME: Why do we save the state here?
		this_thread->_uninvoke();

		localScheduler.get().updateState();
		Scheduler::suspendCurrent();
		Scheduler::unassociate(this_thread.get());

		runOnStack([] (Continuation cont, SyscallImageAccessor image,
				Thread *thread, frg::unique_lock<Mutex> lock) {
			scrubStack(image, cont);
			auto *scheduler = &localScheduler.get();

			ObserveQueue queue;
			queue.splice(queue.end(), thread->_observeQueue);

			lock.unlock();

			// Run observer callbacks before re-scheduling (as callbacks may unblock threads).
			while(!queue.empty()) {
				auto node = queue.pop_front();
				async::execution::set_value(node->receiver,
						frg::make_tuple(Error::threadExited, 0, kIntrNull));
			}

			scheduler->updateQueue();
			scheduler->forceReschedule();
			scheduler->commitReschedule();
		}, getCpuData()->detachedStack.base(), image, this_thread.get(), std::move(lock));
	}else if(this_thread->_pendingSignal == kSigInterrupt) {
		if(logRunStates)
			infoLogger() << "thor: " << (void *)this_thread.get()
					<< " was (asynchronously) interrupted" << frg::endlog;

		this_thread->_updateRunTime();
		this_thread->_runState = kRunInterrupted;
		this_thread->_lastInterrupt = kIntrRequested;
		++this_thread->_stateSeq;
		this_thread->_pendingSignal = kSigNone;
		saveExecutor(&this_thread->_executor, image);
		this_thread->_uninvoke();

		localScheduler.get().updateState();
		Scheduler::suspendCurrent();

		runOnStack([] (Continuation cont, SyscallImageAccessor image,
				Thread *thread, frg::unique_lock<Mutex> lock) {
			scrubStack(image, cont);
			auto *scheduler = &localScheduler.get();

			ObserveQueue queue;
			queue.splice(queue.end(), thread->_observeQueue);
			auto sequence = thread->_stateSeq;

			lock.unlock();

			// Run observer callbacks before re-scheduling (as callbacks may unblock threads).
			while(!queue.empty()) {
				auto node = queue.pop_front();
				async::execution::set_value(node->receiver,
						frg::make_tuple(Error::success, sequence, kIntrRequested));
			}

			scheduler->updateQueue();
			scheduler->forceReschedule();
			scheduler->commitReschedule();
		}, getCpuData()->detachedStack.base(), image, this_thread.get(), std::move(lock));
	}else if(auto assignedCpu = this_thread->_lbCb->getAssignedCpu(); assignedCpu != getCpuData()) {
		// Handle thread migration due to load balancing.
		assert(assignedCpu);
		if(logMigration)
			infoLogger() << "thor: " << (void *)this_thread.get()
					<< " is moved to CPU " << assignedCpu->cpuIndex << frg::endlog;

		this_thread->_updateRunTime();
		this_thread->_runState = kRunSuspended;
		saveExecutor(&this_thread->_executor, image);
		localScheduler.get().update();
		Scheduler::suspendCurrent();
		this_thread->_uninvoke();
		Scheduler::unassociate(this_thread.get());

		auto *newScheduler = &localScheduler.get(assignedCpu);
		Scheduler::associate(this_thread.get(), newScheduler);
		Scheduler::resume(this_thread.get());
		localScheduler.get().forceReschedule();

		runOnStack([] (Continuation cont, SyscallImageAccessor image,
				frg::unique_lock<Mutex> lock) {
			scrubStack(image, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), image, std::move(lock));
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

	if (thread->_runState != kRunBlocked && thread->_runState != kRunInterruptableBlocked)
		return;
	
	if(logRunStates)
		infoLogger() << "thor: " << (void *)thread.get()
				<< " is deferred (via unblock)" << frg::endlog;

	thread->_updateRunTime();
	thread->_runState = kRunDeferred;
	Scheduler::resume(thread.get());
}

void Thread::killOther(smarter::borrowed_ptr<Thread> thread) {
	thread->_kill();
}

void Thread::interruptOther(smarter::borrowed_ptr<Thread> thread) {
	auto irq_lock = frg::guard(&irqMutex());
	bool unblock;

	{
		auto lock = frg::guard(&thread->_mutex);

		// TODO: Perform the interrupt immediately if possible.
		// assert(thread->_pendingSignal == kSigNone);

		thread->_pendingSignal = kSigInterrupt;

		// If the thread is blocked and can be interrupted,
		// then unblock it to notify.
		unblock = (thread->_runState == kRunInterruptableBlocked);
	}

	if(unblock)
		unblockOther(thread);
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

	thread->_updateRunTime();
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
		_universe{std::move(universe)}, _addressSpace{std::move(address_space)} {
	_lastRunTimeUpdate = getClockNanos();
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
	auto *cpuData = getCpuData();
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
	_updateRunTime();
	_runState = kRunActive;

	lock.unlock();

	_userContext.migrate(cpuData);
	AddressSpace::activate(_addressSpace);
	cpuData->executorContext = &_executorContext;
	cpuData->activeThread = self;
	restoreExecutor(&_executor);
}

void Thread::handlePreemption(IrqImageAccessor image) {
	doHandlePreemption(image.inManipulableDomain(), image);
}
void Thread::handlePreemption(FaultImageAccessor image) {
	doHandlePreemption(!image.inKernelDomain(), image);
}
void Thread::handlePreemption(SyscallImageAccessor image) {
	doHandlePreemption(true, image);
}

template<typename ImageAccessor>
void Thread::doHandlePreemption(bool inManipulableDomain, ImageAccessor image) {
	assert(!intsAreEnabled());
	assert(getCurrentThread().get() == this);
	assert(image.iplState()->current < ipl::schedule);

	auto *scheduler = &localScheduler.get();

	scheduler->update();
	if(scheduler->maybeReschedule()) {
		auto lock = frg::guard(&_mutex);

		if(logRunStates)
			infoLogger() << "thor: " << (void *)this << " is deferred" << frg::endlog;

		assert(_runState == kRunActive);
		_updateRunTime();
		if(inManipulableDomain) {
			_runState = kRunSuspended;
		}else{
			_runState = kRunDeferred;
		}
		saveExecutor(&_executor, image);
		_uninvoke();

		runOnStack([] (Continuation cont, ImageAccessor image, frg::unique_lock<Mutex> lock) {
			scrubStack(image, cont);
			lock.unlock();
			localScheduler.get().commitReschedule();
		}, getCpuData()->detachedStack.base(), image, std::move(lock));
	}else{
		scheduler->renewSchedule();
	}
}

void Thread::_updateRunTime() {
	auto now = getClockNanos();
	assert(now >= _lastRunTimeUpdate);
	auto elapsed = now - _lastRunTimeUpdate;
	if (_runState == kRunActive || _runState == kRunSuspended || _runState == kRunDeferred) {
		_loadRunnable += elapsed;
	} else {
		// TODO: Terminated counts as not runnable; we may want to revisit this.
		assert(
		    _runState == kRunBlocked || _runState == kRunInterrupted || _runState == kRunTerminated
		    || _runState == kRunInterruptableBlocked
		);
		_loadNotRunnable += elapsed;
	}
	_lastRunTimeUpdate = now;
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
		_updateRunTime();
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

void Thread::updateLoad() {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	_updateRunTime();

	uint64_t factor = 0;
	// Protect against division by zero.
	if (_loadRunnable)
		factor = (_loadRunnable << loadShift) / (_loadRunnable + _loadNotRunnable);
	_loadLevel.store(factor, std::memory_order_relaxed);
}

void Thread::decayLoad(uint64_t decayFactor, int decayScale) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	// Apply a decay factor. Since this affects both numerator and denominator of the load level,
	// the load level is not immediately affected by this decay.
	auto decayTime = [&] (uint64_t t) -> uint64_t { return (t * decayFactor) >> decayScale; };
	_loadRunnable = decayTime(_loadRunnable);
	_loadNotRunnable = decayTime(_loadNotRunnable);
}

} // namespace thor
