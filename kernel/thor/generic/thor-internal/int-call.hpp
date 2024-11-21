#pragma once

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
template<typename F>
struct SelfIntCall : SelfIntCallBase {
	SelfIntCall(F f)
	: f_{std::move(f)} {}

protected:
	void invoke_() override {
		f_();
	}

private:
	F f_;
};

} // namespace thor
