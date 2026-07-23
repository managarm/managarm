#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <hel.h>
#include <hel-syscalls.h>

#include "testsuite.hpp"

namespace {

constexpr size_t testStackSize = 64 * 1024;
alignas(16) std::array<std::byte, testStackSize> stoppedThreadStack;
std::atomic<int> stoppedThreadFirstCpu{-1};
std::atomic<int> stoppedThreadFinalCpu{-1};
std::atomic<bool> stoppedThreadDone{false};
std::atomic<int> stoppedThreadTargetCpu{-1};
int stoppedThreadPark = 0;

[[noreturn]] void stoppedThreadEntry() {
	int cpu = -1;
	assert(helGetCurrentCpu(&cpu) == kHelErrNone);
	// helGetCurrentCpu() samples before its return path consumes cpuMigration.
	stoppedThreadFirstCpu.store(cpu, std::memory_order_release);

	for(size_t i = 0; i < 10'000; ++i) {
		assert(helYield() == kHelErrNone);
		assert(helGetCurrentCpu(&cpu) == kHelErrNone);
		if(cpu == stoppedThreadTargetCpu.load(std::memory_order_acquire)) {
			stoppedThreadFinalCpu.store(cpu, std::memory_order_release);
			stoppedThreadDone.store(true, std::memory_order_release);
			break;
		}
	}

	while(true)
		assert(helFutexWait(&stoppedThreadPark, 0, -1) == kHelErrNone);
}

void *stoppedThreadStackTop() {
	auto top = reinterpret_cast<uintptr_t>(stoppedThreadStack.data() + testStackSize);
	top &= ~uintptr_t{0xF};
	top -= sizeof(uintptr_t);
	*reinterpret_cast<uintptr_t *>(top) = 0;
	return reinterpret_cast<void *>(top);
}

} // namespace

DEFINE_TEST(affinity_current_thread, ([] {
	std::array<uint8_t, 128> mask{};
	size_t actualSize = 0;
	assert(helGetAffinity(kHelThisThread, mask.data(), mask.size(), &actualSize)
			== kHelErrNone);
	assert(actualSize > 0 && actualSize <= mask.size());
	assert(helSetAffinity(kHelThisThread, mask.data(), actualSize) == kHelErrNone);
}));

DEFINE_TEST(affinity_stopped_thread, ([] {
	std::array<uint8_t, 128> allowedMask{};
	size_t actualSize = 0;
	assert(helGetAffinity(kHelThisThread, allowedMask.data(), allowedMask.size(), &actualSize)
			== kHelErrNone);
	assert(actualSize > 0 && actualSize <= allowedMask.size());

	int initialCpu = -1;
	assert(helGetCurrentCpu(&initialCpu) == kHelErrNone);
	int targetCpu = -1;
	for(size_t i = 0; i < actualSize * 8; ++i)
		if((allowedMask[i / 8] & (1 << (i % 8))) && static_cast<int>(i) != initialCpu) {
			targetCpu = static_cast<int>(i);
			break;
		}
	// There is no distinct target CPU on UP systems.
	if(targetCpu < 0)
		return;

	std::array<uint8_t, 128> targetMask{};
	targetMask[targetCpu / 8] = static_cast<uint8_t>(1 << (targetCpu % 8));
	stoppedThreadFirstCpu.store(-1, std::memory_order_relaxed);
	stoppedThreadFinalCpu.store(-1, std::memory_order_relaxed);
	stoppedThreadDone.store(false, std::memory_order_relaxed);
	stoppedThreadTargetCpu.store(targetCpu, std::memory_order_release);

	HelHandle thread;
	assert(helCreateThread(kHelNullHandle, kHelNullHandle, kHelAbiSystemV,
			reinterpret_cast<void *>(stoppedThreadEntry), stoppedThreadStackTop(),
			kHelThreadStopped, &thread) == kHelErrNone);
	assert(helSetAffinity(thread, targetMask.data(), actualSize) == kHelErrNone);
	assert(helResume(thread) == kHelErrNone);

	for(size_t i = 0; i < 10'000
			&& !stoppedThreadDone.load(std::memory_order_acquire); ++i)
		assert(helYield() == kHelErrNone);
	assert(stoppedThreadDone.load(std::memory_order_acquire));
	assert(stoppedThreadFirstCpu.load(std::memory_order_acquire) == initialCpu);
	assert(stoppedThreadFinalCpu.load(std::memory_order_acquire) == targetCpu);
}));
