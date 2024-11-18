#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>

extern "C" [[noreturn]] void eirEnterKernel(uintptr_t satp, uint64_t entryPtr, uint64_t stackPtr);

namespace eir {

extern uint64_t pml4;

void enterKernel() {
	constexpr uint64_t modeSv48 = 9;
	uint64_t satp = (pml4 >> 12) | (modeSv48 << 60);
	eirEnterKernel(satp, kernelEntry, 0xFFFF'FE80'0001'0000);
}

} // namespace eir
