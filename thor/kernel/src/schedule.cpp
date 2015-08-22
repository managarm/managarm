
#include "kernel.hpp"

namespace traits = frigg::traits;

namespace thor {

frigg::util::LazyInitializer<ThreadQueue> scheduleQueue;

KernelUnsafePtr<Thread> getCurrentThread() {
	return getCpuContext()->currentThread;
}

KernelSharedPtr<Thread> resetCurrentThread() {
	ASSERT(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	ASSERT(cpu_context->currentThread);

	KernelUnsafePtr<Thread> thread = cpu_context->currentThread;
	thread->deactivate();
	return traits::move(cpu_context->currentThread);
}

void dropCurrentThread() {
	ASSERT(!intsAreEnabled());
	resetCurrentThread();
	doSchedule();
}

void enterThread(KernelSharedPtr<Thread> &&thread) {
	ASSERT(!intsAreEnabled());
	auto cpu_context = getCpuContext();
	ASSERT(!cpu_context->currentThread);

	thread->activate();
	cpu_context->currentThread = traits::move(thread);
	restoreThisThread();
}

void doSchedule() {
	ASSERT(!intsAreEnabled());
	ASSERT(!getCpuContext()->currentThread);
	
	while(scheduleQueue->empty()) {
		enableInts();
		halt();
		disableInts();
	}

	enterThread(scheduleQueue->removeFront());
}

void enqueueInSchedule(KernelSharedPtr<Thread> &&thread) {
	scheduleQueue->addBack(traits::move(thread));
}

} // namespace thor

