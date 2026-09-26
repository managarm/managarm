#pragma once

#include <stdint.h>
#include <array>

namespace thor {

// Load is tracked as an exponentially weighted moving average of a signal s(t),
// i.e., s = full while a thread is runnable and 0 otherwise,
// with decay rate lambda = ln(2) / half-life.
// If s has the constant value c during an interval of length dt, the average evolves as
// L(t + dt) = f L(t) + (1 - f) c with f = 2^(-dt / half-life).
// This is linear in L and c and f only depends on dt. Hence, the sum of several averages (at the same time)
// can be advanced using only that sum and the sum of the signals; this is exact (up to rounding)
// as long as none of the signals changes during dt.

// Shift for fixed point numbers that represent the load level.
constexpr int loadShift = 10;

// The half-life of the load is (1 << loadHalfLifeShift) nanoseconds (i.e., around 537 ms).
// TODO: Re-evaluate once migrations are subject to hysteresis; a shorter half-life reacts faster
//       but, without hysteresis, it makes the balancer move threads back and forth.
constexpr int loadHalfLifeShift = 29;

// Extra fractional bits of per-thread averages; otherwise, frequent short updates would be lost.
constexpr int loadFractionShift = 20;

namespace load_detail {

// ln(2) as 0.64 fixed point number, via ln(2) = -ln(1 - 1/2) = sum_{k >= 1} 1 / (k 2^k).
consteval uint64_t computeLn2() {
	uint64_t sum = 0;
	for (int k = 1; k < 64; ++k)
		sum += (UINT64_C(1) << (64 - k)) / k;
	return sum;
}

// 2^(-k/32) as 32.32 fixed point numbers. Sums the Taylor series exp(-x) = sum_n (-x)^n / n!
// for x = k ln(2) / 32 in 64.64 fixed point, computing each term from the previous one.
consteval std::array<uint64_t, 32> computeDecayTable() {
	std::array<uint64_t, 32> table{};
	for (int k = 0; k < 32; ++k) {
		auto x = (static_cast<__uint128_t>(computeLn2()) * k) >> 5;
		__uint128_t sum = static_cast<__uint128_t>(1) << 64;
		__uint128_t term = sum;
		for (int n = 1; term; ++n) {
			term = ((term * x) >> 64) / n;
			if (n & 1)
				sum -= term;
			else
				sum += term;
		}
		table[k] = static_cast<uint64_t>((sum + (UINT64_C(1) << 31)) >> 32);
	}
	return table;
}

constexpr std::array<uint64_t, 32> decayTable = computeDecayTable();

// ln(2) as 0.32 fixed point number.
constexpr uint64_t ln2 = (computeLn2() + (UINT64_C(1) << 31)) >> 32;

constexpr uint64_t one = UINT64_C(1) << 32;

} // namespace load_detail

// 2^(-elapsed / half-life) as 32.32 fixed point number.
// A distinct type such that it cannot be confused with the elapsed time.
struct LoadDecayFactor {
	uint64_t value;
};

// Writes elapsed / half-life = n + j / 32 + q / half-life (for integers n, 0 <= j < 32, q < half-life / 32)
// and computes 2^(-n) * 2^(-j/32) * exp(-q ln(2) / half-life).
constexpr LoadDecayFactor loadDecayFactor(uint64_t elapsed) {
	using namespace load_detail;

	auto halvings = elapsed >> loadHalfLifeShift;
	if (halvings >= 32)
		return {0};

	// The table covers 2^(-j/32). For y = q ln(2) / half-life < ln(2) / 32,
	// exp(-y) is approximately 1 - y + y^2 / 2 with a relative error below y^3 / 6 < 2^-19.
	constexpr int tableShift = loadHalfLifeShift - 5;
	// The product of q < 2^tableShift and ln(2) in 0.32 fixed point must fit into 64 bits.
	static_assert(tableShift >= 0 && tableShift <= 32);
	auto remainder = elapsed & ((UINT64_C(1) << loadHalfLifeShift) - 1);
	// y as 0.32 fixed point number.
	auto x = ((remainder & ((UINT64_C(1) << tableShift) - 1)) * ln2) >> loadHalfLifeShift;
	auto taylor = one - x + ((x * x) >> 33);

	auto factor = static_cast<uint64_t>(
		(static_cast<__uint128_t>(decayTable[remainder >> tableShift]) * taylor) >> 32
	);
	return {factor >> halvings};
}

// Advances an average by the time that corresponds to factor, during which the signal had the constant
// value signal. Returns f load + (1 - f) signal (rounded) for f = factor. As a convex combination,
// the result lies between load and signal; in particular, averages of signals in [0, full] stay in [0, full].
// Averages that advance by the same time can share the factor.
constexpr uint64_t advanceLoad(uint64_t load, uint64_t signal, LoadDecayFactor factor) {
	using namespace load_detail;

	return static_cast<uint64_t>(
		(static_cast<__uint128_t>(load) * factor.value
			+ static_cast<__uint128_t>(signal) * (one - factor.value)
			+ (one >> 1)) >> 32
	);
}

namespace load_detail {

constexpr uint64_t halfLife = UINT64_C(1) << loadHalfLifeShift;

static_assert(loadDecayFactor(0).value == one);
static_assert(loadDecayFactor(halfLife).value == one / 2);
static_assert(loadDecayFactor(32 * halfLife).value == 0);

// Each Taylor segment meets the next table entry up to the approximation error of 2^-19.
// For j = 32, the next piece is the halving, which yields exactly 1/2. If c is the computed value of ln(2),
// the last segment ends at exp(-c); hence, this also checks that exp(-c) = 1/2, i.e., c = ln(2).
consteval bool isContinuous() {
	constexpr uint64_t tableStep = halfLife / 32;
	for (uint64_t j = 1; j <= 32; ++j) {
		auto before = loadDecayFactor(j * tableStep - 1).value;
		auto at = loadDecayFactor(j * tableStep).value;
		if (before < at || before - at > (one >> 19))
			return false;
	}
	return true;
}
static_assert(isContinuous());

// A zero interval leaves the average unchanged.
static_assert(advanceLoad(1000, UINT64_C(1) << (loadShift + loadFractionShift), loadDecayFactor(0)) == 1000);

} // namespace load_detail

// Load of a thread at a point in time.
struct ThreadLoad {
	// Extrapolates the load under the assumption that the thread did not change its state.
	ThreadLoad at(uint64_t now) const {
		uint64_t elapsed = now > timestamp ? now - timestamp : 0;
		constexpr uint64_t full = UINT64_C(1) << loadShift;
		auto factor = loadDecayFactor(elapsed);
		return {
			.timestamp = now,
			.runnable = advanceLoad(runnable, isRunnable ? full : 0, factor),
			.running = advanceLoad(running, isRunning ? full : 0, factor),
			.isRunnable = isRunnable,
			.isRunning = isRunning,
		};
	}

	uint64_t timestamp{0};
	// Fraction of time that the thread is runnable (i.e., running or waiting for a CPU).
	uint64_t runnable{0};
	// Fraction of time that the thread is running.
	uint64_t running{0};
	bool isRunnable{false};
	bool isRunning{false};
	// frg::seqlock_cell requires an object representation without padding bits.
	uint8_t padding[6]{};
};

} // namespace thor
