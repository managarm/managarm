#include <riscv/sbi.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/system.hpp>

namespace thor {

void initializeArchitecture() {
	sbi::dbcn::writeString("Hello RISC-V world from thor\n");
	setupBootCpuContext();
}

} // namespace thor
