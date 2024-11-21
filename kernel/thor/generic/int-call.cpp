#include <thor-internal/int-call.hpp>

namespace thor {

void SelfIntCallBase::runScheduledCalls() {
	// Atomically take all objects from the linked list.
	auto head = getCpuData()->selfIntCallPtr.exchange(
		nullptr, std::memory_order_relaxed
	);
	while (head) {
		auto current = head;
		head = std::exchange(current->next_, nullptr);

		// The call can be re-scheduled immediate after we clear the scheduled flag.
		// However, re-scheduling it immediate cannot cause reentrancy due to !intsAreEnabled.
		assert(current->scheduled_.test(std::memory_order_relaxed));
		std::atomic_signal_fence(std::memory_order_release);
		current->scheduled_.clear(std::memory_order_relaxed);
		current->invoke_();
	}
}

void SelfIntCallBase::schedule() {
	auto cpuData = getCpuData();
	if (scheduled_.test_and_set(std::memory_order_relaxed))
		return;
	std::atomic_signal_fence(std::memory_order_acquire);
	// Push this object onto a lock-free singly linked list.
	next_ = cpuData->selfIntCallPtr.load(std::memory_order_relaxed);
	while (true) {
		bool success = cpuData->selfIntCallPtr.compare_exchange_strong(
			next_, this, std::memory_order_relaxed
		);
		if (success)
			break;
	}
	sendSelfCallIpi();
}

} // namespace thor
