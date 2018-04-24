#ifndef PROTOCOLS_CLOCK_DEFS_HPP
#define PROTOCOLS_CLOCK_DEFS_HPP

#include <stdint.h>

struct TrackerPage {
	uint64_t seqlock;
	int32_t state;
	int32_t padding;
	int64_t refClock;
	int64_t baseRealtime;
};

#endif // PROTOCOLS_CLOCK_DEFS_HPP
