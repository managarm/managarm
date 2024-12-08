#include <assert.h>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/memory-layout.hpp>

extern "C" [[noreturn]] void eirEnterKernel(uintptr_t satp, uint64_t entryPtr, uint64_t stackPtr);

namespace eir {

extern uint64_t pml4;
constinit RiscvConfig riscvConfig;

bool patchArchSpecificManagarmElfNote(unsigned int type, frg::span<char> desc) {
	if (type == elf_note_type::riscvConfig) {
		if (desc.size() != sizeof(RiscvConfig))
			panicLogger() << "RiscvConfig size does not match ELF note" << frg::endlog;
		// Config must be known by now.
		assert(riscvConfig.numPtLevels);
		memcpy(desc.data(), &riscvConfig, sizeof(RiscvConfig));
		return true;
	}
	return false;
}

void enterKernel() {
	uint64_t mode = 8 + (riscvConfig.numPtLevels - 3);
	uint64_t satp = (pml4 >> 12) | (mode << 60);
	eirEnterKernel(satp, kernelEntry, getKernelStackPtr());
}

} // namespace eir
