#include <assert.h>
#include <core/clock.hpp>
#include <frg/safe_int.hpp>
#include <hel.h>
#include <print>

#include "clocks.hpp"

namespace posix {

uint64_t convertToNanos(const timespec &ts, clockid_t clock, bool relative) {
	auto nanosSafe = frg::safe_int{static_cast<uint64_t>(ts.tv_sec)} * frg::safe_int<uint64_t>{1'000'000'000}
			+ frg::safe_int{static_cast<uint64_t>(ts.tv_nsec)};
	uint64_t nanos;
	if (!nanosSafe.into(nanos))
		nanos = UINT64_MAX;

	if(relative) {
		uint64_t now;
		HEL_CHECK(helGetClock(&now));
		uint64_t r;
		if(!(frg::safe_int{now} + frg::safe_int{nanos}).into(r))
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
