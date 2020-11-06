#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/cpu.hpp>

namespace thor {

void initializeArchitecture() {
	setupBootCpuContext();
	initializeIrqVectors();
}

}
