
#include "kernel.hpp"

namespace traits = frigg::traits;
namespace debug = frigg::debug;

namespace thor {

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

const Word kRflagsBase = 0x1;
const Word kRflagsIf = 0x200;

Thread::Thread(SharedPtr<Universe, KernelAlloc> &&universe,
		SharedPtr<AddressSpace, KernelAlloc> &&address_space,
		SharedPtr<RdFolder, KernelAlloc> &&directory,
		bool kernel_thread)
: p_universe(universe), p_addressSpace(address_space),
		p_directory(directory), p_kernelThread(kernel_thread) {
	memset(&p_state, 0, sizeof(ThorRtThreadState));

	memset(&p_tss, 0, sizeof(frigg::arch_x86::Tss64));
	frigg::arch_x86::initializeTss64(&p_tss);
	p_tss.ist1 = (uintptr_t)kernelStackBase + kernelStackLength;
}

UnsafePtr<Universe, KernelAlloc> Thread::getUniverse() {
	return p_universe;
}
UnsafePtr<AddressSpace, KernelAlloc> Thread::getAddressSpace() {
	return p_addressSpace;
}
UnsafePtr<RdFolder, KernelAlloc> Thread::getDirectory() {
	return p_directory;
}

bool Thread::isKernelThread() {
	return p_kernelThread;
}

void Thread::enableIoPort(uintptr_t port) {
	p_tss.ioBitmap[port / 8] &= ~(1 << (port % 8));
}

ThorRtThreadState &Thread::accessState() {
	return p_state;
}

void switchThread(UnsafePtr<Thread, KernelAlloc> thread) {
	*currentThread = SharedPtr<Thread, KernelAlloc>(thread);
	
	thread->p_addressSpace->switchTo();
	thorRtUserContext = &thread->p_state;
	thorRtEnableTss(&thread->p_tss);
}

// --------------------------------------------------------
// ThreadQueue
// --------------------------------------------------------

ThreadQueue::ThreadQueue() { }

bool ThreadQueue::empty() {
	return p_front.get() == nullptr;
}

void ThreadQueue::addBack(SharedPtr<Thread, KernelAlloc> &&thread) {
	// setup the back pointer before moving the thread pointer
	UnsafePtr<Thread, KernelAlloc> back = p_back;
	p_back = thread;

	// move the thread pointer into the queue
	if(empty()) {
		p_front = traits::move(thread);
	}else{
		thread->p_previousInQueue = back;
		back->p_nextInQueue = traits::move(thread);
	}
}

SharedPtr<Thread, KernelAlloc> ThreadQueue::removeFront() {
	ASSERT(!empty());
	
	// move the front and second element out of the queue
	SharedPtr<Thread, KernelAlloc> front = traits::move(p_front);
	SharedPtr<Thread, KernelAlloc> next = traits::move(front->p_nextInQueue);
	front->p_previousInQueue = UnsafePtr<Thread, KernelAlloc>();

	// fix the pointers to previous elements
	if(next.get() == nullptr) {
		p_back = UnsafePtr<Thread, KernelAlloc>();
	}else{
		next->p_previousInQueue = UnsafePtr<Thread, KernelAlloc>();
	}

	// move the second element back to the queue
	p_front = traits::move(next);

	return front;
}

SharedPtr<Thread, KernelAlloc> ThreadQueue::remove(UnsafePtr<Thread, KernelAlloc> thread) {
	// move the successor out of the queue
	SharedPtr<Thread, KernelAlloc> next = traits::move(thread->p_nextInQueue);
	UnsafePtr<Thread, KernelAlloc> previous = thread->p_previousInQueue;
	thread->p_previousInQueue = UnsafePtr<Thread, KernelAlloc>();

	// fix pointers to previous elements
	if(p_back.get() == thread.get()) {
		p_back = previous;
	}else{
		next->p_previousInQueue = previous;
	}
	
	// move the successor back to the queue
	// move the thread out of the queue
	SharedPtr<Thread, KernelAlloc> reference;
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

