#pragma once

#include <async/recurring-event.hpp>
#include <thor-internal/arch-generic/ints.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

struct SelfIntCallBase {
	// Called by the interrupt handler.
	// Pre-condition: !intsAreEnabled().
	static void runScheduledCalls();

	// Schedule this call to be invoked in interrupt context.
	// Pre-condition: !intsAreEnabled().
	void schedule();

protected:
	virtual void invoke_() = 0;

private:
	std::atomic_flag scheduled_{false};
	SelfIntCallBase *next_{nullptr};
};

// Helper class to schedule a function in interrupt context.
// For example, this can be used to "escape" NMI or exception contexts.
//
// Note: Do not deallocate this object while it is scheduled.
// However, it is safe to re-schedule it while it is already scheduled.
// If this is done, multiple calls to schedule() are coalesced.
// It is also safe to schedule it on multiple CPUs; the function can then run concurrently.
template<typename F>
struct SelfIntCall : SelfIntCallBase {
	constexpr SelfIntCall(F f)
	: f_{std::move(f)} {}

protected:
	void invoke_() override {
		f_();
	}

private:
	F f_;
};

// Event with the raise path wired through SelfIntCall;
// i.e., it can be raised in all contexts in which we can also call into SelfIntCall.
// The event stays pending until it is cleared.
struct SelfIntEvent {
	// Pre-condition: !intsAreEnabled().
	void raise() {
		assert(!intsAreEnabled());
		if(!pending_.exchange(true, std::memory_order_acq_rel))
			call_.schedule();
	}

	// Returns true if the event was pending.
	bool clear() {
		return pending_.exchange(false, std::memory_order_acq_rel);
	}

	// Waits if the event is currently pending.
	// Callers must be able to deal with spurious wakeups.
	auto wait() {
		return event_.async_wait_if([this] {
			return !pending_.load(std::memory_order_acquire);
		});
	}

private:
	struct Raise {
		void operator() () {
			self->event_.raise();
		}

		SelfIntEvent *self;
	};

	std::atomic<bool> pending_{false};
	async::recurring_event event_;
	SelfIntCall<Raise> call_{Raise{this}};
};

} // namespace thor
