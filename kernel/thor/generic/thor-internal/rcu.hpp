#pragma once

#include <optional>

#include <async/recurring-event.hpp>
#include <async/wait-group.hpp>
#include <frg/allocation.hpp>
#include <frg/list.hpp>
#include <smarter.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/rcu-base.hpp>

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

// shared_ptr integration with RCU.
template<typename T, typename Deallocator>
struct rcu_meta_object final
: smarter::meta_object_base, RcuCallable {
	template<typename... Args>
	rcu_meta_object(Deallocator d, Args &&... args)
	: smarter::meta_object_base{&finalize_, &finalize_weak_}, d_{std::move(d)} {
		ctr().setup(smarter::adopt_rc, 1);
		weak_ctr().setup(smarter::adopt_rc, 1);
		box_.initialize(std::forward<Args>(args)...);
	}

	T *get() {
		return box_.get();
	}

private:
	static void finalize_(smarter::meta_object_base *base) {
		auto self = static_cast<rcu_meta_object *>(base);
		submitRcu(self, &rcu_callback_);
	}

	static void finalize_weak_(smarter::meta_object_base *base) {
		auto self = static_cast<rcu_meta_object *>(base);
		self->d_(self);
	}

	static void rcu_callback_(RcuCallable *rcuBase) {
		auto self = static_cast<rcu_meta_object *>(rcuBase);
		self->box_.destruct();
		if(self->weak_ctr().decrement_and_check_if_zero())
			self->finalize_weak();
	}

	frg::manual_box<T> box_;
	Deallocator d_;
};

// Like smarter::allocate_shared() but calls the destructor via submitRcu().
template<typename T, typename Allocator, typename... Args>
smarter::shared_ptr<T> allocate_rcu_shared(Allocator alloc, Args &&... args) {
	using meta_type = rcu_meta_object<T, smarter::allocator_deallocator<Allocator>>;
	auto meta = frg::construct<meta_type>(
		alloc,
		smarter::allocator_deallocator<Allocator>{alloc},
		std::forward<Args>(args)...
	);
	return smarter::shared_ptr<T>{smarter::adopt_rc, meta->get(), smarter::default_rc_policy{meta}};
}

} // namespace
