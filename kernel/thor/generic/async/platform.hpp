#pragma once

#include <thor-internal/kernel_heap.hpp>

#define LIBASYNC_THREAD_LOCAL

namespace async::platform {
	using mutex = thor::IrqSpinlock;

	inline void panic(const char *) { __builtin_trap(); }
};
