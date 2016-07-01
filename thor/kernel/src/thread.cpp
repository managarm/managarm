
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

// FIXME: atomicity
uint64_t nextGlobalThreadId = 1;

Thread::Thread(KernelSharedPtr<Universe> &&universe,
		KernelSharedPtr<AddressSpace> &&address_space,
		KernelSharedPtr<RdFolder> &&directory)
: globalThreadId(nextGlobalThreadId++), flags(0),
		_runState(kRunActive), // FIXME: do not use the active run state here
		p_universe(universe), p_addressSpace(address_space), p_directory(directory) {
//	infoLogger->log() << "[" << globalThreadId << "] New thread!" << frigg::EndLog();
}

Thread::~Thread() {
	while(!_observeQueue.empty()) {
		assert(!"Fix join");
/*		JoinRequest request = _observeQueue.removeFront();

		UserEvent event(UserEvent::kTypeJoin, request.submitInfo);
		EventHub::Guard hub_guard(&request.eventHub->lock);
		request.eventHub->raiseEvent(hub_guard, frigg::move(event));
		hub_guard.unlock();*/
	}
}

KernelUnsafePtr<Universe> Thread::getUniverse() {
	return p_universe;
}
KernelUnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return p_addressSpace;
}
KernelUnsafePtr<RdFolder> Thread::getDirectory() {
	return p_directory;
}

void Thread::transitionToFault() {
	assert(_runState == kRunActive);
	_runState = kRunFaulted;

	while(!_observeQueue.empty()) {
		frigg::SharedPtr<AsyncObserve> observe = _observeQueue.removeFront();
		AsyncOperation::complete(frigg::move(observe));
	}
}

void Thread::resume() {
	assert(_runState == kRunFaulted);
	_runState = kRunActive;
}

void Thread::submitObserve(KernelSharedPtr<AsyncObserve> observe) {
	_observeQueue.addBack(frigg::move(observe));
}

// --------------------------------------------------------
// ThreadQueue
// --------------------------------------------------------

ThreadQueue::ThreadQueue() { }

bool ThreadQueue::empty() {
	return !p_front;
}

void ThreadQueue::addBack(KernelSharedPtr<Thread> thread) {
	// setup the back pointer before moving the thread pointer
	KernelUnsafePtr<Thread> back = p_back;
	p_back = thread;

	// move the thread pointer into the queue
	if(empty()) {
		p_front = frigg::move(thread);
	}else{
		thread->p_previousInQueue = back;
		back->p_nextInQueue = frigg::move(thread);
	}
}

KernelSharedPtr<Thread> ThreadQueue::removeFront() {
	assert(!empty());
	
	// move the front and second element out of the queue
	KernelSharedPtr<Thread> front = frigg::move(p_front);
	KernelSharedPtr<Thread> next = frigg::move(front->p_nextInQueue);
	front->p_previousInQueue = KernelUnsafePtr<Thread>();

	// fix the pointers to previous elements
	if(!next) {
		p_back = KernelUnsafePtr<Thread>();
	}else{
		next->p_previousInQueue = KernelUnsafePtr<Thread>();
	}

	// move the second element back to the queue
	p_front = frigg::move(next);

	return front;
}

KernelSharedPtr<Thread> ThreadQueue::remove(KernelUnsafePtr<Thread> thread) {
	// move the successor out of the queue
	KernelSharedPtr<Thread> next = frigg::move(thread->p_nextInQueue);
	KernelUnsafePtr<Thread> previous = thread->p_previousInQueue;
	thread->p_previousInQueue = KernelUnsafePtr<Thread>();

	// fix pointers to previous elements
	if(p_back.get() == thread.get()) {
		p_back = previous;
	}else{
		next->p_previousInQueue = previous;
	}
	
	// move the successor back to the queue
	// move the thread out of the queue
	KernelSharedPtr<Thread> reference;
	if(p_front.get() == thread.get()) {
		reference = frigg::move(p_front);
		p_front = frigg::move(next);
	}else{
		reference = frigg::move(previous->p_nextInQueue);
		previous->p_nextInQueue = frigg::move(next);
	}

	return reference;
}

} // namespace thor

