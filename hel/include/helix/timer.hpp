#ifndef HELIX_TIMEOUT_HPP
#define HELIX_TIMEOUT_HPP

#include <helix/ipc.hpp>
#include <async/cancellation.hpp>

namespace helix {

template<typename F>
struct TimeoutCallback {
	TimeoutCallback(uint64_t duration, F function)
	: _function{std::move(function)} {
		_runTimer(duration);
	}

	TimeoutCallback(const TimeoutCallback &other) = delete;

	TimeoutCallback &operator= (const TimeoutCallback &other) = delete;

	async::result<void> retire() {
		_cancelTimer.cancel();
		return _promise.async_get();
	}

private:
	async::detached _runTimer(uint64_t duration) {
		uint64_t tick;
		HEL_CHECK(helGetClock(&tick));

		helix::AwaitClock await;
		auto &&submit = helix::submitAwaitClock(&await, tick + duration,
				helix::Dispatcher::global());
		auto async_id = await.asyncId();

		{
			async::cancellation_callback cb{_cancelTimer, [&] {
				HEL_CHECK(helCancelAsync(helix::Dispatcher::global().acquire(),
						async_id));
			}};
			co_await submit.async_wait();
		}

		if(await.error() != kHelErrCancelled) {
			HEL_CHECK(await.error());
			_function();
		}

		_promise.set_value();
	}

	F _function;
	async::cancellation_event _cancelTimer;
	async::promise<void> _promise;
};

struct TimeoutCancellation {
	TimeoutCancellation(uint64_t duration, async::cancellation_event &ev)
	:_tb{duration, Functor{&ev}} {
	}

	async::result<void> retire() {
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

inline async::result<void> sleepFor(uint64_t duration) {
	uint64_t tick;
	HEL_CHECK(helGetClock(&tick));

	helix::AwaitClock await;
	auto &&submit = helix::submitAwaitClock(&await, tick + duration,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(await.error());
}

// Returns true if the operation succeeded, or false if it timed out
template<typename F> requires (std::is_invocable_r_v<bool, F>)
async::result<bool> kindaBusyWait(uint64_t timeoutNs, F cond) {
	uint64_t startNs, currNs;
	HEL_CHECK(helGetClock(&startNs));

	do {
		if (std::invoke(cond))
			co_return true;

		// Sleep for 5ms (TODO: make adaptive?)
		co_await sleepFor(5'000'000);

		HEL_CHECK(helGetClock(&currNs));
	} while (currNs < startNs + timeoutNs);

	co_return std::invoke(cond);
}

// Returns true if the operation succeeded, or false if it timed out
template<typename F> requires (std::is_invocable_r_v<bool, F>)
bool busyWaitUntil(uint64_t timeoutNs, F cond) {
	uint64_t startNs, currNs;
	HEL_CHECK(helGetClock(&startNs));

	do {
		if (std::invoke(cond))
			return true;

		HEL_CHECK(helGetClock(&currNs));
	} while (currNs < startNs + timeoutNs);

	return std::invoke(cond);
}

} // namespace helix

#endif // HELIX_TIMEOUT_HPP
