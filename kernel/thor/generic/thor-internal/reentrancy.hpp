#pragma once

#include <thor-internal/cpu-data.hpp>

namespace thor {

struct ReentrancySafeSpinlock {
	constexpr ReentrancySafeSpinlock() = default;

	ReentrancySafeSpinlock(const ReentrancySafeSpinlock &) = delete;

	ReentrancySafeSpinlock &operator= (const ReentrancySafeSpinlock &) = delete;

	// TODO: Consider switching to a fair implementation for this.
	//       For example, we could let unlock() search per-CPU state for a next owner.
	void lock() {
		auto cpu = getCpuData();
		uint32_t st{~UINT32_C(0)};
		while (true) {
			auto success = state_.compare_exchange_weak(
				st,
				cpu->cpuIndex,
				std::memory_order_acquire,
				std::memory_order_relaxed
			);
			if (success)
				break;
			while ((st = state_.load(std::memory_order_relaxed)) != ~UINT32_C(0))
				frg::detail::loophint();
		}
	}

	void unlock() {
		state_.store(~UINT32_C(0), std::memory_order_release);
	}

	CpuData *owner() {
		auto st = state_.load(std::memory_order_relaxed);
		if (st == ~UINT32_C(0))
			return nullptr;
		return getCpuData(st);
	}

private:
	std::atomic<uint32_t> state_{~UINT32_C(0)};
};

} // namespace thor
