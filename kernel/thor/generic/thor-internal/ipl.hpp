#pragma once

#include <thor-internal/cpu-data.hpp>

namespace thor {

inline Ipl currentIpl() {
	auto state = getCpuData()->iplState.load(std::memory_order_relaxed);
	return state.current;
}

inline void deferToIplLowerThan(Ipl l) {
	assert(l > 0);
	getCpuData()->iplDeferred.fetch_or(
		static_cast<IplMask>(1) << (l - 1),
		std::memory_order_relaxed
	);
}

void handleIplDeferred(Ipl current, Ipl ceiling);

template<Ipl L>
struct IplGuard {
	IplGuard() {
		auto cpuData = getCpuData();
		auto state = cpuData->iplState.load(std::memory_order_relaxed);

		// Otherwise, this guard is taken in a context where it cannot be taken.
		assert(state.context <= L);

		if (state.current >= L)
			return;

		cpuData->iplState.store(
			IplState{
				.context = state.context,
				.current = L,
			},
			std::memory_order_relaxed);

		// Perform (w, rw) fence to prevent re-ordering of future accesses with the iplState store.
		std::atomic_signal_fence(std::memory_order_seq_cst);

		previous_ = state.current;
	}

	IplGuard(const IplGuard &) = delete;

	~IplGuard() {
		auto cpuData = getCpuData();
		auto state = cpuData->iplState.load(std::memory_order_relaxed);

		if (previous_ == ipl::bad) {
			assert(state.current >= L);
			return;
		}
		assert(previous_ < L);

		// Perform (rw, w) fence to prevent re-ordering of past accesses with the iplState store.
		std::atomic_signal_fence(std::memory_order_release);
		cpuData->iplState.store(
			IplState{
				.context = state.context,
				.current = previous_,
			},
			std::memory_order_relaxed);

		// Perform (w, rw) fence to prevent re-ordering of iplState store and iplDeferred load.
		std::atomic_signal_fence(std::memory_order_seq_cst);

		auto deferred = cpuData->iplDeferred.load(std::memory_order_relaxed);
		auto mask = (~static_cast<IplMask>(0)) << previous_;
		if (deferred & mask) [[unlikely]]
			handleIplDeferred(previous_, L);
	}

	IplGuard operator=(const IplGuard &) = delete;

private:
	Ipl previous_{ipl::bad};
};

} // namespace thor
