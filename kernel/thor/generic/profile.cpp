#include <thor-internal/arch/pmc-amd.hpp>
#include <thor-internal/arch/pmc-intel.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/profile.hpp>
#include <thor-internal/service_helpers.hpp>

namespace thor {

bool wantKernelProfile = false;
frigg::LazyInitializer<LogRingBuffer> globalProfileRing;

void initializeProfile() {
	if(!wantKernelProfile)
		return;

	if(!(getCpuData()->profileFlags & PlatformCpuData::profileIntelSupported)
			&& !(getCpuData()->profileFlags & PlatformCpuData::profileAmdSupported)) {
		frigg::infoLogger() << "\e[31m" "thor: Kernel profiling was requested but"
				" no hardware support is available" "\e[39m" << frigg::endLog;
		return;
	}

	void *profileMemory = kernelAlloc->allocate(1 << 20);
	globalProfileRing.initialize(reinterpret_cast<uintptr_t>(profileMemory), 1 << 20);

	// Dump the per-CPU profiling data to the global ring buffer.
	// TODO: Start one such fiber per CPU.
	KernelFiber::run([=] {
		getCpuData()->localProfileRing = frg::construct<SingleContextRecordRing>(*kernelAlloc);

		if(getCpuData()->profileFlags & PlatformCpuData::profileIntelSupported) {
			initializeIntelPmc();
			getCpuData()->profileMechanism.store(ProfileMechanism::intelPmc,
					std::memory_order_release);
			setIntelPmc();
		}else{
			assert(getCpuData()->profileFlags & PlatformCpuData::profileAmdSupported);
			getCpuData()->profileMechanism.store(ProfileMechanism::amdPmc,
					std::memory_order_release);
			setAmdPmc();
		}

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
