#include <cassert>
#include <cstdint>
#include <cstdio>

#include <hel.h>
#include <hel-syscalls.h>
#include <helix/clock.hpp>

#include "testsuite.hpp"

DEFINE_TEST(clockMatchesSyscall, ([] {
	if(!helix::hasUserspaceClock()) {
		printf("kernel-tests: clockMatchesSyscall test was NOT exercised"
				" (the kernel exports no clock that userspace can read)\n");
	}

	// The userspace-accessible clock must exactly reproduce the helGetClock()-provided time.
	for(int i = 0; i < 1000; ++i) {
		uint64_t before;
		HEL_CHECK(helGetClock(&before));
		auto nanos = helix::getClock();
		uint64_t after;
		HEL_CHECK(helGetClock(&after));

		assert(nanos >= before);
		assert(nanos <= after);
	}
}))

DEFINE_TEST(clockIsMonotone, ([] {
	auto previous = helix::getClock();
	for(int i = 0; i < 1000; ++i) {
		auto nanos = helix::getClock();
		assert(nanos >= previous);
		previous = nanos;
	}
}))
