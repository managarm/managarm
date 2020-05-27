#include "../arch/x86/pmc-amd.hpp"
#include "fiber.hpp"
#include "profile.hpp"
#include "service_helpers.hpp"

namespace thor {

bool wantKernelProfile = false;
frigg::LazyInitializer<LogRingBuffer> globalProfileRing;

void initializeProfile() {
	if(!wantKernelProfile)
		return;

	if(!(getCpuData()->profileFlags & PlatformCpuData::profileAmdSupported)) {
		frigg::infoLogger() << "\e[31m" "thor: Kernel profiling was requested but"
				" no hardware support is available" "\e[39m" << frigg::endLog;
		return;
	}

	void *profileMemory = kernelAlloc->allocate(1 << 20);
	globalProfileRing.initialize(reinterpret_cast<uintptr_t>(profileMemory), 1 << 20);

	// Dump the per-CPU profiling data to the global ring buffer.
	// TODO: Start one such fiber per CPU.
	KernelFiber::run([=] {
		assert(getCpuData()->profileFlags & PlatformCpuData::profileAmdSupported);
		getCpuData()->localProfileRing = frg::construct<SingleContextRecordRing>(*kernelAlloc);
		getCpuData()->profileMechanism.store(ProfileMechanism::amdPmc,
				std::memory_order_release);
		setAmdPmc();

		uint64_t deqPtr = 0;
		while(true) {
			char buffer[128];
			auto [success, newPtr, size] = getCpuData()->localProfileRing->dequeueAt(
					deqPtr, buffer, 128);
			deqPtr = newPtr;
			if(!success) {
				fiberSleep(1'000'000);
				continue;
			}
			assert(size);
			assert(size < 128);
			for(size_t i = 0; i < size; ++i)
				globalProfileRing->enqueue(buffer[i]);
		}
	});
}

LogRingBuffer *getGlobalProfileRing() {
	return globalProfileRing.get();
}

} // namespace thor
