#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/timer.hpp>

namespace {

bool kernelInEl2 = false;

}

namespace thor {

void initializeArchitecture() {
	uint64_t el;
	asm volatile("mrs %0, CurrentEL" : "=r"(el));
	kernelInEl2 = (el >> 2) == 2;

	setupBootCpuContext();
	initializeTimers();
	initializeIrqVectors();
}

bool isKernelInEl2() { return kernelInEl2; }

} // namespace thor
