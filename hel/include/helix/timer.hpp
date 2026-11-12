#pragma once

#include <helix/clock.hpp>
#include <helix/ipc.hpp>
#include <async/algorithm.hpp>
#include <async/cancellation.hpp>
#include <async/result.hpp>
#include <async/oneshot-event.hpp>

#include <functional>
#include <optional>
#include <type_traits>

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

// The getClock() value after durationNs (none for no duration, or if it does not fit into the clock).
inline std::optional<uint64_t> deadlineAfter(std::optional<uint64_t> durationNs) {
	if(!durationNs)
		return std::nullopt;
	uint64_t deadline;
	if(__builtin_add_overflow(getClock(), *durationNs, &deadline))
		return std::nullopt;
	return deadline;
}

// Runs the sender that f(ct) returns and cancels ct at the deadline (a getClock() value), if any.
// The sender reports the cancellation in its value (e.g., recurring_event::async_wait() yields false).
template<typename F>
requires std::invocable<F, async::cancellation_token>
		&& async::Sender<std::invoke_result_t<F, async::cancellation_token>>
auto withTimeout(F f, std::optional<uint64_t> deadline)
		-> async::result<typename std::invoke_result_t<F, async::cancellation_token>::value_type> {
	using T = typename std::invoke_result_t<F, async::cancellation_token>::value_type;

	if(!deadline)
		co_return co_await f(async::cancellation_token{});

	// race_and_cancel() only takes senders without values.
	auto timer = async::lambda([&](async::cancellation_token ct) -> async::result<void> {
		co_await sleepUntil(*deadline, ct);
	});
	if constexpr (std::is_void_v<T>) {
		co_await async::race_and_cancel(
			async::lambda([&](async::cancellation_token ct) -> async::result<void> {
				co_await f(ct);
			}),
			std::move(timer)
		);
	} else {
		std::optional<T> value;
		co_await async::race_and_cancel(
			async::lambda([&](async::cancellation_token ct) -> async::result<void> {
				value.emplace(co_await f(ct));
			}),
			std::move(timer)
		);
		co_return std::move(*value);
	}
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
