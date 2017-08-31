#ifndef THOR_GENERIC_TIMER_HPP
#define THOR_GENERIC_TIMER_HPP

#include <frg/pairing_heap.hpp>

namespace thor {

struct PrecisionTimerNode {
	PrecisionTimerNode(uint64_t deadline)
	: deadline{deadline} { }

	// The timer subsystem drops its references to the node before this call.
	virtual void onElapse() = 0;

	uint64_t deadline;

	frg::pairing_heap_hook<PrecisionTimerNode> hook;
};

} // namespace thor

#endif // THOR_GENERIC_TIMER_HPP
