#pragma once

#include <assert.h>
#include <frg/spinlock.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

void initializeIrqVectors();

inline bool intsAreEnabled() {
	assert("STUB: intsAreEnabled()" && false);
}

inline void enableInts() {
	assert("STUB: enableInts()" && false);
}

inline void disableInts() {
	assert("STUB: disableInts()" && false);
}

inline void halt() {
	assert("STUB: halt()" && false);
}

inline void suspendSelf() {
	assert("STUB: suspendSelf()" && false);
}

inline void sendPingIpi(int id) {
	assert("STUB: sendPingIpi()" && false);
	(void)id;
}

inline void sendShootdownIpi() {
	assert("STUB: sendShootdownIpi()" && false);
}

} // namespace thor
