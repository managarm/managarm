#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <atomic>
#include <concepts>
#include <optional>
#include <vector>

#include <async/algorithm.hpp>
#include <async/basic.hpp>
#include <async/result.hpp>
#include <async/run-queue.hpp>
#include <frg/scope_exit.hpp>
#include <helix/ipc.hpp>

namespace helix {

// Pool of threads that each drive a thread-local helix::Dispatcher.
// The DispatcherPool is owned by the thread that creates it.
// The owner of the DispatcherPool is adopted as its first pool member.
struct DispatcherPool {
	static DispatcherPool &global();

	DispatcherPool(const DispatcherPool &) = delete;

	DispatcherPool &operator= (const DispatcherPool &) = delete;

	// Index of the calling thread within the pool, or std::nullopt if the calling thread is not a pool member.
	std::optional<size_t> thisThread();

	// Maximal number of members that the pool can ever have.
	size_t maxThreads();

	// Fixes the number of pool threads.
	// Must be called before the pool is brought up.
	void setThreadCount(size_t count) {
		assert(async::current_run_queue_context() == ownerContext_
				&& "setThreadCount() must be called from the owner thread");
		assert(!live_.load(std::memory_order_relaxed)
				&& "setThreadCount() must be called before bring-up");
		assert(count > 0);
		threadCount_ = count;
	}

	// Drives the calling thread's helix::Dispatcher until the sender completes and returns the sender's value.
	// Brings up the pool on first call.
	// Must not be called from a worker thread and must not be nested.
	template<async::Sender S>
	typename S::value_type blockOn(S sender) {
		enter_();
		inBlockOn_ = true;
		frg::scope_exit leaveOnExit{[&] {
			inBlockOn_ = false;
		}};
		return async::run(std::move(sender), currentDispatcher);
	}

	// Detaches a sender into the pool.
	// The pool must have been brought up before.
	template<async::Sender S>
	requires std::same_as<typename S::value_type, void>
	void detach(S sender) {
		// There is no run queue to detach to before bring-up.
		if(!live_.load(std::memory_order_acquire))
			abort();
		auto index = next_.fetch_add(1, std::memory_order_relaxed) % members_.size();
		async::detach_on(members_[index], onRunQueue_(std::move(sender)));
	}

private:
	DispatcherPool();

	// Workaround for detach_on() since coroutines still start inline right now.
	// TODO: Remove this when libasync also moves initial suspend to the queue.
	template<async::Sender S>
	static async::result<void> onRunQueue_(S sender) {
		co_await async::invocable([] { });
		co_await std::move(sender);
	}

	void enter_();

	// Context of the owner thread.
	async::run_queue_context *ownerContext_{nullptr};
	// Number of threads that the pool uses.
	size_t threadCount_{0};
	// Member run queues. Immutable after bring-up.
	std::vector<async::run_queue *> members_;
	// Index of the thread that detach() targets next.
	std::atomic<size_t> next_{0};
	// True after bring-up.
	std::atomic<bool> live_{false};
	// Whether we are currently in blockOn() or not.
	bool inBlockOn_{false};
};

} // namespace helix
