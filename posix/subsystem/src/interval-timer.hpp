#pragma once

#include <async/cancellation.hpp>
#include <helix/timer.hpp>
#include <stdint.h>

namespace posix {

struct IntervalTimer {
	IntervalTimer(uint64_t initial, uint64_t interval)
		: initial_(initial), interval_(interval) {
	}

	virtual ~IntervalTimer() {}

	// Static function that arms the timer and keeps it alive until expiration or cancellation.
	static async::detached arm(std::shared_ptr<IntervalTimer> timer);

	void getTime(uint64_t &initial, uint64_t &interval);

	void cancel() {
		cancelEvt_.cancel();
	}

	// Called when the timer expires. The success argument indicates whether the timer
	// expired (true) or if the wait was cancelled (false).
	virtual void raise(bool success) = 0;

	// Called after final timer expiration.
	virtual void expired() = 0;

protected:
	uint64_t initial_ = 0;
	uint64_t interval_ = 0;
	uint64_t nextExpiration_ = 0;

private:
	async::cancellation_event cancelEvt_;
};

} // namespace posix
