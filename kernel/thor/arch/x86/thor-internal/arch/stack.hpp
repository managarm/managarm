#pragma once

#include <stdint.h>

namespace thor {

template <typename F>
inline void walkStack(void *basePtr, F functor) {
	auto bp = reinterpret_cast<uintptr_t *>(basePtr);

	for (uintptr_t ip = bp[1];
			bp && reinterpret_cast<uintptr_t>(bp) >= 0xffff800000000000;
			ip = bp[1], bp = reinterpret_cast<uintptr_t *>(bp[0])) {
		functor(ip);
	}
}

template <typename F>
inline void walkThisStack(F functor) {
	void *bp;
	asm volatile ("mov %%rbp, %0" : "=r"(bp));
	walkStack(bp, functor);
}

} // namespace thor
