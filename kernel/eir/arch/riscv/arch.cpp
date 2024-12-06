#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/memory-layout.hpp>

extern "C" [[noreturn]] void eirEnterKernel(uintptr_t satp, uint64_t entryPtr, uint64_t stackPtr);

namespace eir {

extern uint64_t pml4;
constinit RiscvConfig riscvConfig;

void enterKernel() {
	uint64_t mode = 8 + (riscvConfig.numPtLevels - 3);
	uint64_t satp = (pml4 >> 12) | (mode << 60);
	eirEnterKernel(satp, kernelEntry, getKernelStackPtr());
}

} // namespace eir
