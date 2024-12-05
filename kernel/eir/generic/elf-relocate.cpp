#include <eir-internal/arch.hpp>
#include <elf.h>

extern "C" [[gnu::visibility("hidden")]] Elf64_Dyn _DYNAMIC[];

namespace eir {

extern "C" void eirRelocate() {
	auto base = reinterpret_cast<uintptr_t>(&eirImageFloor);
	uintptr_t relaAddr = 0;
	uintptr_t relaSize = 0;

	for (auto *dyn = _DYNAMIC; dyn->d_tag != DT_NULL; ++dyn) {
		switch (dyn->d_tag) {
			case DT_RELA:
				relaAddr = dyn->d_ptr + base;
				break;
			case DT_RELASZ:
				relaSize = dyn->d_val;
				break;
			default:
				break;
		}
	}

	for (uintptr_t i = 0; i < relaSize; i += sizeof(Elf64_Rela)) {
		auto *rela = reinterpret_cast<const Elf64_Rela *>(relaAddr + i);
#if defined(__aarch64__)
		if (ELF64_R_TYPE(rela->r_info) != R_AARCH64_RELATIVE)
			__builtin_trap();
#elif defined(__riscv) && __riscv_xlen == 64
		if (ELF64_R_TYPE(rela->r_info) != R_RISCV_RELATIVE)
			__builtin_trap();
#else
#error "Platform does not support PIE in Eir"
#endif
		*reinterpret_cast<Elf64_Addr *>(rela->r_offset + base) = base + rela->r_addend;
	}
}

} // namespace eir
