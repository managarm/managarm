#ifndef PROTOCOLS_FS_DEFS_HPP
#define PROTOCOLS_FS_DEFS_HPP

#include <stdint.h>

namespace protocols::fs {

struct StatusPage {
	uint64_t seqlock;
	uint64_t sequence;
	int flags;
	int status;
};

} // namespace protocols::fs

#endif // PROTOCOLS_FS_DEFS_HPP
