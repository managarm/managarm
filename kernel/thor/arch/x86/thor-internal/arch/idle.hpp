#pragma once

#include <stdint.h>

namespace thor {

enum class IdleInstruction {
	hlt,
	mwait
};

struct IdleMethod {
	IdleInstruction instruction = IdleInstruction::hlt;
	// Only used by IdleInstruction::mwait.
	uint32_t mwaitHint = 0;
};

} // namespace thor
