#pragma once

#include <optional>

#include <async/recurring-event.hpp>
#include <async/wait-group.hpp>
#include <frg/list.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

// RcuEngine implements an RCU mechanism where disabling scheduling acts as an RCU read-side lock.
struct RcuEngine {
	coroutine<void> barrier();

private:
	// Sequence number of the RCU state transition.
	static constexpr uint64_t stateSeq = (UINT64_C(1) << 63) - 1;
	// This bit is set when there is an ongoing state transition.
	static constexpr uint64_t stateBusy = UINT64_C(1) << 63;

	std::atomic<uint64_t> state_{0};
	// This event is raised whenever the busy bit transitions to clear.
	async::recurring_event seqEvent_;

	// Used to wait until the transition is done on all CPUs.
	async::wait_group transitionWg_{0};
};

struct RcuCallable {
	friend struct RcuDispatcher;

private:
	void (*call_)(RcuCallable *);
	frg::default_list_hook<RcuCallable> hook_;
};

void setRcuOnline(CpuData *cpu);

void submitRcu(RcuCallable *callable, void (*call)(RcuCallable *));

// Policy class for frigg::rcu_radixtree.
struct RcuPolicy {
	template<typename T, typename D>
	struct obj_base : private RcuCallable {
		void retire(D d = D()) {
			d_ = std::move(d);
			submitRcu(this, [] (RcuCallable *base) {
				auto self = static_cast<obj_base *>(base);
				(*self->d_)(static_cast<T *>(self));
			});
		}

	private:
		std::optional<D> d_;
	};
};

} // namespace
