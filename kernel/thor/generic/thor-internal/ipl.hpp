#pragma once

#include <frg/mutex.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/cpu-data.hpp>

namespace thor {

inline Ipl contextIpl() {
	return getCpuData()->contextIpl.load(std::memory_order_relaxed);
}

inline Ipl currentIpl() {
	return getCpuData()->currentIpl.load(std::memory_order_relaxed);
}

[[noreturn]] void panicOnIllegalIplEntry(Ipl newIpl, Ipl currentIpl);

inline void iplSave(IplState &savedIpl) {
	auto cpuData = getCpuData();
	savedIpl.current = cpuData->currentIpl.load(std::memory_order_relaxed);
	savedIpl.context = cpuData->contextIpl.load(std::memory_order_relaxed);
}

// Raise context IPL. Takes saved IPL for error checking.
inline void iplEnterContext(Ipl newIpl, IplState savedIpl) {
	if(newIpl < ipl::maximal) {
		if (!(savedIpl.current < newIpl)) [[unlikely]]
			panicOnIllegalIplEntry(newIpl, savedIpl.current);
	}
	auto cpuData = getCpuData();
	cpuData->currentIpl.store(newIpl, std::memory_order_relaxed);
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->contextIpl.store(newIpl, std::memory_order_relaxed);
	// Perform (w, rw) fence to prevent re-ordering of future accesses with the IPL stores.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Restore (= lowering) context + current IPL.
inline void iplLeaveContext(IplState savedIpl) {
	auto cpuData = getCpuData();
	// Perform (rw, w) fence to prevent re-ordering of past accesses with the IPL stores.
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->contextIpl.store(savedIpl.context, std::memory_order_relaxed);
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->currentIpl.store(savedIpl.current, std::memory_order_relaxed);
	// Perform (w, rw) fence to prevent re-ordering of the IPL stores with future accesses.
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
	auto current = cpuData->currentIpl.load(std::memory_order_relaxed);

	if (current >= raisedIpl)
		return ipl::bad;

	cpuData->currentIpl.store(raisedIpl, std::memory_order_relaxed);

	// Perform (w, rw) fence to prevent re-ordering of future accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_seq_cst);

	return current;
}

inline void iplLower(Ipl lowerIpl) {
	auto cpuData = getCpuData();
	auto current = cpuData->currentIpl.load(std::memory_order_relaxed);

	assert(lowerIpl != ipl::bad);
	assert(lowerIpl <= current);

	// Perform (rw, w) fence to prevent re-ordering of past accesses with the iplState store.
	std::atomic_signal_fence(std::memory_order_release);
	cpuData->currentIpl.store(lowerIpl, std::memory_order_relaxed);

	// Perform (w, rw) fence to prevent re-ordering of iplState store and iplDeferred load.
	std::atomic_signal_fence(std::memory_order_seq_cst);
}

template<Ipl L>
struct IplGuard {
	IplGuard() {
		auto cpuData = getCpuData();
		auto context = cpuData->contextIpl.load(std::memory_order_relaxed);

		// Otherwise, this guard is taken in a context where it cannot be taken.
		assert(context <= L);

		previous_ = iplRaise(L);
	}

	IplGuard(const IplGuard &) = delete;

	~IplGuard() {
		auto cpuData = getCpuData();
		auto current = cpuData->currentIpl.load(std::memory_order_relaxed);

		if (previous_ == ipl::bad) {
			assert(current >= L);
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

struct IrqMutex {
private:
	static constexpr unsigned int enableBit = 0x8000'0000;

public:
	IrqMutex() = default;

	IrqMutex(const IrqMutex &) = delete;

	void lock() {
		auto cpuData = getCpuData();
		// We maintain the following invariants:
		// * Properly nested lock()/unlock() pairs restore IRQs to the original state.
		// * If we observe cpuData->intState > 0 then IRQs are disabled.
		//
		// NMIs and faults can always interrupt us but that is
		// not a problem because of the first invariant.
		auto s = cpuData->intState.load(std::memory_order_acquire);
		if(!s) {
			auto e = intsAreEnabled();
			if(e) {
				disableInts();
				cpuData->intState.store(enableBit | 1, std::memory_order_relaxed);
			}else{
				cpuData->intState.store(1, std::memory_order_relaxed);
			}
		}else{
			// Because of the second invariant we do not need to examine the IRQ state here.
			assert(s & ~enableBit);
			cpuData->intState.store(s + 1, std::memory_order_release);
		}
	}

	void unlock() {
		auto cpuData = getCpuData();
		auto s = cpuData->intState.load(std::memory_order_relaxed);
		assert(s & ~enableBit);
		if((s & ~enableBit) == 1) {
			cpuData->intState.store(0, std::memory_order_release);
			if(s & enableBit)
				enableInts();
		}else{
			cpuData->intState.store(s - 1, std::memory_order_release);
		}
	}

	unsigned int nesting() {
		auto cpuData = getCpuData();
		return cpuData->intState.load(std::memory_order_relaxed) & ~enableBit;
	}
};

struct StatelessIrqLock {
	StatelessIrqLock()
	: _locked{false} {
		lock();
	}

	StatelessIrqLock(frg::dont_lock_t)
	: _locked{false} { }

	StatelessIrqLock(const StatelessIrqLock &) = delete;

	~StatelessIrqLock() {
		if(_locked)
			unlock();
	}

	StatelessIrqLock &operator= (const StatelessIrqLock &) = delete;

	void lock() {
		assert(!_locked);
		_enabled = intsAreEnabled();
		disableInts();
		_locked = true;
	}

	void unlock() {
		assert(_locked);
		if(_enabled)
			enableInts();
		_locked = false;
	}

private:
	bool _locked;
	bool _enabled;
};

inline IrqMutex globalIrqMutex;

inline IrqMutex &irqMutex() {
	return globalIrqMutex;
}

} // namespace thor
