#pragma once

#include <frg/spinlock.hpp>

namespace thor {

void initializeProcessorEarly();

inline bool intsAreEnabled() {
	// TODO
	return false;
}

inline void enableInts() {
	// TODO
}

inline void disableInts() {
	// TODO
}

inline void halt() {
	asm volatile ("wfi");
}

void suspendSelf();

void sendPingIpi(int id);

} // namespace thor
