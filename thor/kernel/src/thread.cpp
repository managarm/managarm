
#include "kernel.hpp"

namespace traits = frigg::traits;
namespace debug = frigg::debug;

namespace thor {

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

Thread::Thread(KernelSharedPtr<Universe> &&universe,
		KernelSharedPtr<AddressSpace> &&address_space,
		KernelSharedPtr<RdFolder> &&directory,
		bool kernel_thread)
: p_universe(universe), p_addressSpace(address_space),
		p_directory(directory), p_kernelThread(kernel_thread) { }

KernelUnsafePtr<Universe> Thread::getUniverse() {
	return p_universe;
}
KernelUnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return p_addressSpace;
}
KernelUnsafePtr<RdFolder> Thread::getDirectory() {
	return p_directory;
}

bool Thread::isKernelThread() {
	return p_kernelThread;
}

void Thread::enableIoPort(uintptr_t port) {
	p_saveState.threadTss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

void Thread::activate() {
	p_addressSpace->activate();
	p_saveState.activate();
}

void Thread::deactivate() {
	p_saveState.deactivate();
}

ThorRtThreadState &Thread::accessSaveState() {
	return p_saveState;
}

// --------------------------------------------------------
// ThreadQueue
// --------------------------------------------------------

ThreadQueue::ThreadQueue() { }

bool ThreadQueue::empty() {
	return p_front.get() == nullptr;
}

void ThreadQueue::addBack(KernelSharedPtr<Thread> &&thread) {
	// setup the back pointer before moving the thread pointer
	KernelUnsafePtr<Thread> back = p_back;
	p_back = thread;

	// move the thread pointer into the queue
	if(empty()) {
		p_front = traits::move(thread);
	}else{
		thread->p_previousInQueue = back;
		back->p_nextInQueue = traits::move(thread);
	}
}

KernelSharedPtr<Thread> ThreadQueue::removeFront() {
	assert(!empty());
	
	// move the front and second element out of the queue
	KernelSharedPtr<Thread> front = traits::move(p_front);
	KernelSharedPtr<Thread> next = traits::move(front->p_nextInQueue);
	front->p_previousInQueue = KernelUnsafePtr<Thread>();

	// fix the pointers to previous elements
	if(next.get() == nullptr) {
		p_back = KernelUnsafePtr<Thread>();
	}else{
		next->p_previousInQueue = KernelUnsafePtr<Thread>();
	}

	// move the second element back to the queue
	p_front = traits::move(next);

	return front;
}

KernelSharedPtr<Thread> ThreadQueue::remove(KernelUnsafePtr<Thread> thread) {
	// move the successor out of the queue
	KernelSharedPtr<Thread> next = traits::move(thread->p_nextInQueue);
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
		reference = traits::move(p_front);
		p_front = traits::move(next);
	}else{
		reference = traits::move(previous->p_nextInQueue);
		previous->p_nextInQueue = traits::move(next);
	}

	return reference;
}

} // namespace thor

