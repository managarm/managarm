#pragma once

#include <thor-internal/arch/ints.hpp>
#include <thor-internal/cpu-data.hpp>
#include <atomic>
#include <frg/mutex.hpp>
#include <assert.h>

namespace thor {

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
