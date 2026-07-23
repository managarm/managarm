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
constexpr size_t stoppedThreadCount = 2;
alignas(16) std::array<std::byte, testStackSize> stoppedThreadStacks[stoppedThreadCount];
std::atomic<size_t> stoppedThreadStarts{0};
std::atomic<size_t> stoppedThreadDone{0};
std::atomic<int> stoppedThreadFirstCpu[stoppedThreadCount];
std::atomic<int> stoppedThreadFinalCpu[stoppedThreadCount];
std::atomic<int> stoppedThreadTargetCpu{-1};
int stoppedThreadPark = 0;

[[noreturn]] void stoppedThreadEntry() {
	auto index = stoppedThreadStarts.fetch_add(1, std::memory_order_relaxed);
	assert(index < stoppedThreadCount);

	int cpu = -1;
	assert(helGetCurrentCpu(&cpu) == kHelErrNone);
	// helGetCurrentCpu() samples before its return path consumes cpuMigration.
	stoppedThreadFirstCpu[index].store(cpu, std::memory_order_release);

	// The first syscall's return path consumes cpuMigration. Yield once to
	// reach a subsequent safe point, then sample the CPU after that migration.
	assert(helYield() == kHelErrNone);
	assert(helGetCurrentCpu(&cpu) == kHelErrNone);
	stoppedThreadFinalCpu[index].store(cpu, std::memory_order_release);
	stoppedThreadDone.fetch_add(1, std::memory_order_release);

	while(true)
		assert(helFutexWait(&stoppedThreadPark, 0, -1) == kHelErrNone);
}

void *stoppedThreadStackTop(size_t index) {
	auto top = reinterpret_cast<uintptr_t>(stoppedThreadStacks[index].data() + testStackSize);
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

	int targetCpu = -1;
	bool hasOtherCpu = false;
	for(size_t i = 0; i < actualSize * 8; ++i) {
		if(allowedMask[i / 8] & (1 << (i % 8))) {
			if(targetCpu < 0) {
				targetCpu = static_cast<int>(i);
			} else {
				hasOtherCpu = true;
			}
		}
	}
	// There is no distinct initial CPU to test against on UP systems.
	if(!hasOtherCpu)
		return;

	std::array<uint8_t, 128> targetMask{};
	targetMask[targetCpu / 8] = static_cast<uint8_t>(1 << (targetCpu % 8));
	stoppedThreadStarts.store(0, std::memory_order_relaxed);
	stoppedThreadDone.store(0, std::memory_order_relaxed);
	for(size_t i = 0; i < stoppedThreadCount; ++i) {
		stoppedThreadFirstCpu[i].store(-1, std::memory_order_relaxed);
		stoppedThreadFinalCpu[i].store(-1, std::memory_order_relaxed);
	}
	stoppedThreadTargetCpu.store(targetCpu, std::memory_order_release);

	std::array<HelHandle, stoppedThreadCount> threads;
	for(size_t i = 0; i < stoppedThreadCount; ++i) {
		assert(helCreateThread(kHelNullHandle, kHelNullHandle, kHelAbiSystemV,
				reinterpret_cast<void *>(stoppedThreadEntry), stoppedThreadStackTop(i),
				kHelThreadStopped, &threads[i]) == kHelErrNone);
		assert(helSetAffinity(threads[i], targetMask.data(), actualSize) == kHelErrNone);
	}
	for(auto thread : threads)
		assert(helResume(thread) == kHelErrNone);

	for(size_t i = 0; i < 1'000'000
			&& stoppedThreadDone.load(std::memory_order_acquire) != stoppedThreadCount; ++i)
		assert(helYield() == kHelErrNone);
	assert(stoppedThreadDone.load(std::memory_order_acquire) == stoppedThreadCount);

	bool observedDeferredMigration = false;
	for(size_t i = 0; i < stoppedThreadCount; ++i) {
		observedDeferredMigration |= stoppedThreadFirstCpu[i].load(std::memory_order_acquire) != targetCpu;
		assert(stoppedThreadFinalCpu[i].load(std::memory_order_acquire) == targetCpu);
	}
	assert(observedDeferredMigration);
}));
