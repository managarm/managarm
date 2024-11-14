#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/timer.hpp>

namespace thor {

void initializeArchitecture() {
	setupBootCpuContext();
	initializeTimers();
	initializeIrqVectors();
}

}
