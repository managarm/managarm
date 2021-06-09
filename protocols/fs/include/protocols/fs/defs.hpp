#pragma once

#include <stdint.h>

namespace protocols::fs {

struct StatusPage {
	uint64_t seqlock;
	uint64_t sequence;
	int flags;
	int status;
};

} // namespace protocols::fs
