#pragma once

#include <helix/clock.hpp>
#include <helix/ipc.hpp>
#include <async/cancellation.hpp>
#include <async/result.hpp>
#include <async/oneshot-event.hpp>

#include <functional>

namespace helix {

template<typename F>
struct TimeoutCallback {
	TimeoutCallback(uint64_t duration, F function)
	: _function{std::move(function)} {
		_runTimer(duration);
	}

	TimeoutCallback(const TimeoutCallback &other) = delete;

	TimeoutCallback &operator= (const TimeoutCallback &other) = delete;

	auto retire() {
		_cancelTimer.cancel();
		return _ev.wait();
	}

private:
	async::detached _runTimer(uint64_t duration) {
		auto error = co_await helix_ng::awaitClock(getClock() + duration, _cancelTimer);

		if(error != kHelErrCancelled) {
			HEL_CHECK(error);
			_function();
		}

		_ev.raise();
	}

	F _function;
	async::cancellation_event _cancelTimer;
	async::oneshot_primitive _ev;
};

struct TimeoutCancellation {
	TimeoutCancellation(uint64_t duration, async::cancellation_event &ev)
	:_tb{duration, Functor{&ev}} {
	}

	auto retire() {
		return _tb.retire();
	}

private:
	struct Functor {
		void operator() () const {
			ev->cancel();
		}

		async::cancellation_event *ev;
	};

	TimeoutCallback<Functor> _tb;
};

inline async::result<bool> sleepFor(uint64_t duration, async::cancellation_token cancel = {}) {
	auto error = co_await helix_ng::awaitClock(getClock() + duration, cancel);
	if(error == kHelErrCancelled)
		co_return false;
	HEL_CHECK(error);
	co_return true;
}

inline async::result<bool> sleepUntil(uint64_t tick, async::cancellation_token cancelToken) {
	auto error = co_await helix_ng::awaitClock(tick, cancelToken);
	if(error == kHelErrCancelled)
		co_return false;
	HEL_CHECK(error);
	co_return true;
}

// Returns true if the operation succeeded, or false if it timed out
template<typename F> requires (std::is_invocable_r_v<bool, F>)
async::result<bool> kindaBusyWait(uint64_t timeoutNs, F cond) {
	auto startNs = getClock();

	uint64_t currNs;
	do {
		if (std::invoke(cond))
			co_return true;

		// Sleep for 5ms (TODO: make adaptive?)
		co_await sleepFor(5'000'000);

		currNs = getClock();
	} while (currNs < startNs + timeoutNs);

	co_return std::invoke(cond);
}

// Returns true if the operation succeeded, or false if it timed out
template<typename F> requires (std::is_invocable_r_v<bool, F>)
bool busyWaitUntil(uint64_t timeoutNs, F cond) {
	auto startNs = getClock();

	uint64_t currNs;
	do {
		if (std::invoke(cond))
			return true;

		currNs = getClock();
	} while (currNs < startNs + timeoutNs);

	return std::invoke(cond);
}

} // namespace helix
