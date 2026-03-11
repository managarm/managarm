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

struct LocalRcuEngine {
	struct Guard {
		Guard(LocalRcuEngine &engine)
		: engine_{&engine} {
			// Increase in cs_ the number of active critical sections.
			auto cs = engine_->cs_.load(std::memory_order_relaxed);
			while (true) {
				c_ = cs.cycle();

				CriticalSection newCs;
				newCs.x[c_] = cs.x[c_] + 1;
				newCs.x[!c_] = cs.x[!c_];
				auto success = engine_->cs_.compare_exchange_weak(
					cs,
					newCs,
					std::memory_order_acquire,
					std::memory_order_relaxed
				);
				if (success)
					break;
			}
		}

		~Guard() {
			// Decrease in cs_ the number of active critical sections.
			// Raise gpEvent_ if the cycle that we are in became non-active and hit a count of zero.
			auto cs = engine_->cs_.load(std::memory_order_relaxed);
			while (true) {
				assert((cs.x[c_] & CriticalSection::count) > 0);

				CriticalSection newCs;
				newCs.x[c_] = cs.x[c_] - 1;
				newCs.x[!c_] = cs.x[!c_];
				auto success = engine_->cs_.compare_exchange_weak(
					cs,
					newCs,
					std::memory_order_release,
					std::memory_order_relaxed
				);
				if (!success)
					continue;

				if (!(cs.x[c_] & CriticalSection::active)
						&& (cs.x[c_] & CriticalSection::count) == 1) {
					engine_->gpEvent_.raise();
				}
				break;
			}
		}

		Guard(const Guard &) = delete;

		Guard &operator= (const Guard &) = delete;

	private:
		LocalRcuEngine *engine_;
		int c_;
	};

	coroutine<void> barrier();

private:
	// Sequence number of the RCU state transition.
	static constexpr uint64_t stateSeq = (UINT64_C(1) << 63) - 1;
	// This bit is set when there is an ongoing state transition.
	static constexpr uint64_t stateBusy = UINT64_C(1) << 63;

	std::atomic<uint64_t> state_{0};
	// This event is raised whenever the busy bit transitions to clear.
	async::recurring_event seqEvent_;

	// Represents the state of RCU (read side) criticial sections.
	// We store the number of currently active critical sections
	// for two periods, i.e., before and after an ongoing barrier().
	// The active bit is set for the cycle after the current barrier().
	// It is clear for the cycle that the current barrier() is waiting on.
	// If the non-active criticial section count reaches zero, we raise gpEvent_.
	// barrier() cycles the active bit between x[0] and x[1].
	struct alignas(8) CriticalSection {
		static constexpr uint32_t count = (UINT32_C(1) << 31) - 1;
		static constexpr uint32_t active = UINT32_C(1) << 31;

		int cycle() {
			if (x[0] & active)
				return 0;
			assert(x[1] & active);
			return 1;
		}

		uint32_t x[2] = {active, 0};
	};
	static_assert(std::atomic<CriticalSection>::is_always_lock_free);

	std::atomic<CriticalSection> cs_;
	async::recurring_event gpEvent_;
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
