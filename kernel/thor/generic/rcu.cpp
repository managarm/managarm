#include <async/algorithm.hpp>
#include <async/recurring-event.hpp>
#include <async/wait-group.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

namespace {

constexpr bool logRcuCalls = false;

// RcuEngine implements an RCU mechanism where disabling scheduling acts as an RCU read-side lock.
struct RcuEngine {
	// barrier() guarantees that we see at least one quiescent state on all CPUs before returning.
	// A quiescent state for this purpose is a point Q in the execution of CPU C such that:
	// * Scheduling is enabled at Q.
	// * No memory accesses on C that preceeded Q and that executed while scheduling was disabled
	//   can be re-ordered with any memory accesses that follow barrier() on the current thread.
	//
	// Note that to force a quiescent state, it is enough to force scheduling to a work queue
	// of CPU C followed by an appropriate memory barrier.
	coroutine<void> barrier() {
		// We are using states that consist of a sequence number and a busy bit in this
		// implementation. We guarantee correctness through the following properties,
		// where s is the sequence number at barrier() entry:
		// * All CPUs will go through a quiescent state during the transition from state
		//   ((s + 1) | stateBusy) to (s + 1).
		// * At barrier() entry, we are not in ((s + 1) | stateBusy) yet.
		// * We will only return from barrier() once we reach state (s + 1).
		// Hence, it is guaranteed that each CPU goes through a quiescent state before we return.
		//
		// Note that we are either in state s or in (s | stateBusy) at barrier() entry,
		// so there are two possible state transition paths to (s + 1):
		//                    s => ((s + 1) | stateBusy) => (s + 1)
		// (s | stateBusy) => s => ((s + 1) | stateBusy) => (s + 1)

		auto current = state_.load(std::memory_order_relaxed);
		auto s = current & stateSeq;

		// If we are in (s | stateBusy), wait for the transition to s.
		while (current == (s | stateBusy)) {
			co_await seqEvent_.async_wait_if([&] {
				current = state_.load(std::memory_order_relaxed);
				return current == (s | stateBusy);
			});
		}

		// We may need to initiate the transition from s to ((s + 1) | stateBusy) ourselves.
		bool initiate = false;
		if (current == s) {
			initiate = state_.compare_exchange_strong(
				current,
				(s + 1) | stateBusy,
				std::memory_order_relaxed,
				std::memory_order_relaxed
			);
		}
		if (initiate) {
			transitionWg_.add(getCpuCount());
			for (size_t c = 0; c < getCpuCount(); ++c) {
				auto cpu = &cpuData.getFor(c);
				// TODO: We can do this without allocation by putting the operations into a member vector.
				spawnOnWorkQueue(
					Allocator{},
					cpu->generalWorkQueue,
					async::invocable([this] {
						// Perform an explicit fence here since WorkQueue::schedule() may not be strong enough
						// (e.g., when scheduling to the current thread's WQ).
						// It may be possible to weaken the barrier here by specifying
						// the guarantees that WorkQueue::schedule() should provide.
						std::atomic_thread_fence(std::memory_order_seq_cst);
						transitionWg_.done();
					})
				);
			}
			co_await transitionWg_.wait();

			state_.store(s + 1, std::memory_order_relaxed);
			seqEvent_.raise();
		} else {
			assert((current & stateSeq) > s);

			// If another CPU initiated ((s + 1) | stateBusy), wait for transition out of it.
			while (current == ((s + 1) | stateBusy)) {
				co_await seqEvent_.async_wait_if([&] {
					current = state_.load(std::memory_order_relaxed);
					return current == ((s + 1) | stateBusy);
				});
			}
			assert(current == s + 1 || (current & stateSeq) > s + 1);
		}
	}

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

frg::eternal<RcuEngine> rcuEngine;

} // namespace

// Allows the registration of callbacks that run after an RCU barrier.
// This is per-CPU. The calls run on the CPU's generalWorkQueue.
struct RcuDispatcher {
	RcuDispatcher(CpuData *cpu)
	: cpu_{cpu} { }

	void run() {
		runLoop_(enable_detached_coroutine{.wq = cpu_->generalWorkQueue});
	}

	void submit(RcuCallable *callable, void (*call)(RcuCallable *)) {
		callable->call_ = call;

		bool wasEmpty;
		{
			auto lock = frg::guard(&mutex_);
			wasEmpty = queue_.empty();
			queue_.push_back(callable);
		}
		if (wasEmpty)
			pendingEvent_.raise();
	}

private:
	using CallableList = frg::intrusive_list<
		RcuCallable,
		frg::locate_member<
			RcuCallable,
			frg::default_list_hook<RcuCallable>,
			&RcuCallable::hook_
		>
	>;

	void runLoop_(enable_detached_coroutine) {
		while(true) {
			co_await pendingEvent_.async_wait_if([&] {
				auto lock = frg::guard(&mutex_);
				return queue_.empty();
			});

			CallableList collected;
			{
				auto lock = frg::guard(&mutex_);
				collected.splice(collected.end(), queue_);
			}
			if (collected.empty())
				continue;

			co_await rcuEngine->barrier();

			size_t n = 0;
			while (!collected.empty()) {
				auto callable = collected.pop_front();
				callable->call_(callable);
				++n;
			}
			if (logRcuCalls)
				infoLogger() << "thor: " << n << " RCU calls on CPU " << cpu_->cpuIndex << frg::endlog;
		}
	}

	CpuData *cpu_;
	IrqSpinlock mutex_;
	CallableList queue_;
	async::recurring_event pendingEvent_;
};

// TODO: Move to anonymous namespace?
extern PerCpu<RcuDispatcher> rcuDispatcher;
THOR_DEFINE_PERCPU(rcuDispatcher);

void setRcuOnline(CpuData *cpu) {
	rcuDispatcher.get(cpu).run();
}

void submitRcu(RcuCallable *callable, void (*call)(RcuCallable *)) {
	rcuDispatcher.get().submit(callable, call);
}

} // namespace thor
