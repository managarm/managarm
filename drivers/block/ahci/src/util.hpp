#pragma once

#include <async/result.hpp>
#include <helix/memory.hpp>

inline async::result<void> sleepNs(uint64_t time) {
	uint64_t tick;
	HEL_CHECK(helGetClock(&tick));

	helix::AwaitClock awaitClock;
	auto &&submit = helix::submitAwaitClock(&awaitClock, tick + time, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(awaitClock.error());

	co_return;
}

// Returns true iff the operation timed out
template<typename F> requires (std::is_invocable_r_v<bool, F>)
async::result<bool> kindaBusyWait(uint64_t timeoutNs, F cond) {
	uint64_t startNs, currNs;
	HEL_CHECK(helGetClock(&startNs));

	do {
		if (std::invoke(cond))
			co_return false;

		// Sleep for 5ms (TODO: make adaptive?)
		co_await sleepNs(5000000);

		HEL_CHECK(helGetClock(&currNs));
	} while (currNs < startNs + timeoutNs);

	co_return !std::invoke(cond);
}

template<typename T>
uintptr_t virtToPhys(T p) {
	uintptr_t phys;
	HEL_CHECK(helPointerPhysical(reinterpret_cast<void *>(p), &phys));
	return phys;
}
