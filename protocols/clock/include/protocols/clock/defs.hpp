#ifndef PROTOCOLS_CLOCK_DEFS_HPP
#define PROTOCOLS_CLOCK_DEFS_HPP

#include <stdint.h>

struct TrackerPage {
	uint64_t seqlock;
	uint64_t refClock;
	uint64_t baseRealtime;
};

#endif // PROTOCOLS_CLOCK_DEFS_HPP
