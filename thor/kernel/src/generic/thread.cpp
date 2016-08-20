
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Thread
// --------------------------------------------------------

Thread::Thread(KernelSharedPtr<Universe> universe,
		KernelSharedPtr<AddressSpace> address_space,
		KernelSharedPtr<RdFolder> directory)
: flags(0), _runState(kRunActive), // FIXME: do not use the active run state here
		_pendingSignal(kSigNone), _runCount(1),
		_context(kernelStack.base()),
		_universe(universe), _addressSpace(address_space), _directory(directory) {
//	frigg::infoLogger() << "[" << globalThreadId << "] New thread!" << frigg::endLog;
}

Thread::~Thread() {
	assert(!"Thread destructed");
	if(!_observeQueue.empty())
		frigg::infoLogger() << "Fix thread destructor!" << frigg::endLog;
}

Context &Thread::getContext() {
	return _context;
}

KernelUnsafePtr<Universe> Thread::getUniverse() {
	return _universe;
}
KernelUnsafePtr<AddressSpace> Thread::getAddressSpace() {
	return _addressSpace;
}
KernelUnsafePtr<RdFolder> Thread::getDirectory() {
	return _directory;
}

void Thread::signalKill() {
	assert(_pendingSignal == kSigNone);
	_pendingSignal = kSigKill;

	if(_runState == kRunActive)
		return;

	frigg::panicLogger() << "Thread killed in inactive state" << frigg::endLog;
}

auto Thread::pendingSignal() -> Signal {
	return _pendingSignal;
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

} // namespace thor

