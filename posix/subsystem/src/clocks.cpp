#include <assert.h>
#include <core/clock.hpp>
#include <hel.h>
#include <print>

#include "clocks.hpp"

namespace posix {

uint64_t convertToNanos(const timespec &ts, clockid_t clock, bool relative) {
	uint64_t nanos;
	if(__builtin_mul_overflow(static_cast<uint64_t>(ts.tv_sec), 1'000'000'000, &nanos)
			|| __builtin_add_overflow(ts.tv_nsec, nanos, &nanos))
		nanos = UINT64_MAX;

	if(relative) {
		uint64_t now;
		HEL_CHECK(helGetClock(&now));
		uint64_t r;
		if(__builtin_add_overflow(now, nanos, &r))
			return UINT64_MAX;
		return r;
	} else if(clock == CLOCK_REALTIME) {
		uint64_t now;
		HEL_CHECK(helGetClock(&now));

		// Transform real time to time since boot.
		int64_t bootTime = clk::getRealtimeNanos() - now;
		assert(bootTime >= 0);

		if(nanos > static_cast<uint64_t>(bootTime))
			return nanos - bootTime;
		return 0;
	} else if(clock == CLOCK_MONOTONIC) {
		return nanos;
	} else {
		std::println("posix: unhandled clockid {}", clock);
		abort();
	}
}

} // namespace posix
