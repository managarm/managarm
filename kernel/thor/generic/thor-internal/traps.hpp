#pragma once

#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/schedule.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

// Determine whether the fault is an UAR fault, and handle it appropriately if so.
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor);

// Handle conditions and preemption before returning to user mode.
// Precondition: intsAreEnabled(), i.e., irqLock is *not* token.
// Precondition: currentIpl() == ipl::passive.
// Postcondition: irqLock is taken, i.e., !intsAreEnabled().
template<typename ImageAccessor>
void handleThreadReturnToUserMode(ImageAccessor image, StatelessIrqLock &irqLock) {
	auto thisThread = getCurrentThread();
	assert(thisThread);

	while (true) {
		Thread::handleConditions(image);

		irqLock.lock();
		checkThreadPreemption(ipl::passive);
		if (!thisThread->checkConditions())
			break;
		irqLock.unlock();
	}
}

} // namespace thor
