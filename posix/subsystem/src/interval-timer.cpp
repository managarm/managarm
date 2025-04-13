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

void IntervalTimer::getTime(uint64_t &initial, uint64_t &interval) {
	uint64_t now;
	HEL_CHECK(helGetClock(&now));

	if(nextExpiration_ > now)
		initial = nextExpiration_ - now;
	else
		initial = 1;

	interval = interval_;
}

} // namespace posix
