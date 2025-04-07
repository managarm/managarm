#include "interval-timer.hpp"

namespace {

uint64_t add_sat(uint64_t x, uint64_t y) {
	uint64_t r;
	if (__builtin_add_overflow(x, y, &r))
		return UINT64_MAX;
	return r;
}

}

namespace posix {

// Static function that arms the timer and keeps it alive until expiration or cancellation.
async::detached IntervalTimer::arm(std::shared_ptr<IntervalTimer> timer) {
	if(!timer->initial_ && !timer->interval_)
		co_return;

	// Next expiration of the timer.
	timer->nextExpiration_ = timer->initial_;

	if(timer->initial_) {
		bool awaited = co_await helix::sleepUntil(timer->nextExpiration_, timer->cancelEvt_);

		timer->raise(awaited);
		if(!awaited)
			co_return;
	}

	if(!timer->interval_)
		co_return timer->expired();

	while(true) {
		timer->nextExpiration_ = add_sat(timer->nextExpiration_, timer->interval_);
		auto awaited = co_await helix::sleepUntil(timer->nextExpiration_, timer->cancelEvt_);

		timer->raise(awaited);
		if(!awaited)
			co_return;
	}
}

void IntervalTimer::setTime(uint64_t initial, uint64_t interval) {
	reset();
	initial_ = initial;
	interval_ = interval;
}

void IntervalTimer::getTime(timespec &initial, timespec &interval) {
	uint64_t now;
	HEL_CHECK(helGetClock(&now));

	uint64_t left = 0;
	if(initial_ > now)
		left = initial_ - now;

	initial.tv_sec = left / 1'000'000'000;
	initial.tv_nsec = left % 1'000'000'000;
	interval.tv_sec = interval_ / 1'000'000'000;
	interval.tv_nsec = interval_ % 1'000'000'000;
}

void IntervalTimer::getTime(timeval &initial, timeval &interval) {
	uint64_t now;
	HEL_CHECK(helGetClock(&now));

	uint64_t left = 0;
	if(initial_ > now)
		left = initial_ - now;

	initial.tv_sec = left / 1'000'000'000;
	initial.tv_usec = (left % 1'000'000'000) / 1'000;
	interval.tv_sec = interval_ / 1'000'000'000;
	interval.tv_usec = (interval_ % 1'000'000'000) / 1'000;
}

void IntervalTimer::reset() {
	cancelEvt_.cancel();
	nextExpiration_ = 0;
	initial_ = 0;
	interval_ = 0;
}

} // namespace posix
