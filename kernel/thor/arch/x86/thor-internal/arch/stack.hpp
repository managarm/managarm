#pragma once

#include <stdint.h>

namespace thor {

template <typename F>
inline void walkThisStack(F functor) {
	uintptr_t *bp;
	asm volatile ("mov %%rbp, %0" : "=r"(bp));

	for (uintptr_t ip = bp[1];
			bp && reinterpret_cast<uintptr_t>(bp) >= 0xffff800000000000;
			ip = bp[1], bp = reinterpret_cast<uintptr_t *>(bp[0])) {
		functor(ip);
	}
}

} // namespace thor
