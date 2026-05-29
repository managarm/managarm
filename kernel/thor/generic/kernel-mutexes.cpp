#include <thor-internal/block-on.hpp>
#include <thor-internal/executor-context.hpp>
#include <thor-internal/kernel-mutexes.hpp>

namespace thor {

void AdaptiveMutex::lock_() {
	assert(currentIpl() == ipl::noPreemption);

	if (asyncMutex_.try_lock()) [[likely]] {
		ownerCtx_.store(currentExecutorContext(), std::memory_order_relaxed);
		return;
	}
	[[unlikely]] lockSlow_();
}

[[gnu::noinline]] void AdaptiveMutex::lockSlow_() {
	// TTAS in a loop.
	while (true) {
		if (asyncMutex_.test()) {
			if (asyncMutex_.try_lock()) {
				ownerCtx_.store(currentExecutorContext(), std::memory_order_relaxed);
				return;
			}
		} else {
			// If the owner blocks, we also block.
			// Note that this check is not perfectly reliable since reading ownerCtx_
			// and active is not atomic (e.g., the thread may have unlocked and became inactive).
			auto ownerCtx = ownerCtx_.load(std::memory_order_relaxed);
			if (ownerCtx) {
				if (!ownerCtx->active.load(std::memory_order_relaxed))
					break;
			}
			frg::detail::loophint();
		}
	}

	// A nullptr ownerCtx_ may be caused by the owner releasing the lock.
	// Hence, do another try_lock() to avoid blockOn() if the lock has just been released.
	if (asyncMutex_.try_lock()) {
		ownerCtx_.store(currentExecutorContext(), std::memory_order_relaxed);
		return;
	}

	// Snapshot currentExecutorContext() since the lambda below runs in a different context.
	auto thisCtx = currentExecutorContext();
	blockOn(
		// async::transform() ensures that ownerCtx_ is stored *before* unblocking the thread.
		// This is necessary to avoid deadlocks: other threads may try to take the mutex (with
		// preemption disabled) after the previous owner has been unblocked but before it is scheduled.
		async::transform(
			asyncMutex_.async_lock(),
			[&] { ownerCtx_.store(thisCtx, std::memory_order_relaxed); }
		)
	);
}

void AdaptiveMutex::unlock_() {
	assert(currentIpl() == ipl::noPreemption);

	ownerCtx_.store(nullptr, std::memory_order_relaxed);
	asyncMutex_.unlock();
}

} // namespace thor
