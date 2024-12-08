#include <riscv/sbi.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/system.hpp>

namespace thor {

THOR_DEFINE_ELF_NOTE(riscvConfigNote){elf_note_type::riscvConfig, {}};

void initializeArchitecture() {
	sbi::dbcn::writeString("Hello RISC-V world from thor\n");
	setupBootCpuContext();
}

} // namespace thor
