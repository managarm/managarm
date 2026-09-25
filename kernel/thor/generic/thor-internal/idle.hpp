#pragma once

#include <thor-internal/arch-generic/idle.hpp>

namespace thor {

// Selects an idle state of the current CPU and returns the method to enter it.
// Must be called with interrupts disabled at ipl::interrupt.
IdleMethod determineIdleState();

// Updates the idle governor's state at the end of the current idle period (if any).
// Called by the idle task when it wakes up or before it is scheduled away
// (in which case schedulingAway is set).
void noteIdleWakeup(bool schedulingAway);

} // namespace thor
