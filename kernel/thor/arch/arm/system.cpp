#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/timer.hpp>

namespace thor {

void initializeArchitecture() {
	setupBootCpuContext();
	initializeTimers();
	initializeIrqVectors();
}

} // namespace thor
