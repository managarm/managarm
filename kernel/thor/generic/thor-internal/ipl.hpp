#pragma once

#include <thor-internal/cpu-data.hpp>

namespace thor {

inline Ipl contextIpl() {
	auto state = getCpuData()->iplState.load(std::memory_order_relaxed);
	return state.context;
}

inline Ipl currentIpl() {
	auto state = getCpuData()->iplState.load(std::memory_order_relaxed);
	return state.current;
}

[[noreturn]] void panicOnIllegalIplEntry(Ipl newIpl, Ipl currentIpl);

inline void iplSave(IplState &savedIpl) {
	auto cpuData = getCpuData();
	savedIpl = cpuData->iplState.load(std::memory_order_relaxed);
}

// Raise context IPL. Takes saved IPL for error checking.
inline void iplEnterContext(Ipl newIpl, IplState savedIpl) {
	if(newIpl < ipl::maximal) {
		if (!(savedIpl.current < newIpl)) [[unlikely]]
			panicOnIllegalIplEntry(newIpl, savedIpl.current);
	}
	auto cpuData = getCpuData();
	cpuData->iplState.store(
		IplState{
			.context = newIpl,
			.current = newIpl,
		},
		std::memory_order_relaxed);
	// Perform (w, rw) fence to prevent re-ordering of future accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Restore (= lowering) context + current IPL.
inline void iplLeaveContext(IplState savedIpl) {
	auto cpuData = getCpuData();
	// Perform (rw, w) fence to prevent re-ordering of past accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->iplState.store(savedIpl, std::memory_order_relaxed);
	// Perform (w, rw) fence to prevent re-ordering of the iplState store with future accesses.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

inline void deferToIplLowerThan(Ipl l) {
	assert(l > 0);
	getCpuData()->iplDeferred.fetch_or(
		static_cast<IplMask>(1) << (l - 1),
		std::memory_order_relaxed
	);
}

void handleIplDeferred(Ipl current, Ipl ceiling);

// Return the previous IPL or ipl::bad if the IPL was not raised.
inline Ipl iplRaise(Ipl raisedIpl) {
	auto cpuData = getCpuData();
	auto state = cpuData->iplState.load(std::memory_order_relaxed);

	if (state.current >= raisedIpl)
		return ipl::bad;

	cpuData->iplState.store(
		IplState{
			.context = state.context,
			.current = raisedIpl,
		},
		std::memory_order_relaxed);

	// Perform (w, rw) fence to prevent re-ordering of future accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_seq_cst);

	return state.current;
}

inline void iplLower(Ipl lowerIpl) {
	auto cpuData = getCpuData();
	auto state = cpuData->iplState.load(std::memory_order_relaxed);

	assert(lowerIpl != ipl::bad);
	assert(lowerIpl <= state.current);

	// Perform (rw, w) fence to prevent re-ordering of past accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->iplState.store(
		IplState{
			.context = state.context,
			.current = lowerIpl,
		},
		std::memory_order_relaxed);

	// Perform (w, rw) fence to prevent re-ordering of iplState store and iplDeferred load.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

template<Ipl L>
struct IplGuard {
	IplGuard() {
		auto cpuData = getCpuData();
		auto state = cpuData->iplState.load(std::memory_order_relaxed);

		// Otherwise, this guard is taken in a context where it cannot be taken.
		assert(state.context <= L);

		previous_ = iplRaise(L);
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

		iplLower(previous_);

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
