#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/arch/unimplemented.hpp>

namespace thor {

void initializeArchitecture() {
	// setupBootCpuContext();
	// initializeTimers();
	// initializeIrqVectors();
	unimplementedOnRiscv();
}

} // namespace thor
