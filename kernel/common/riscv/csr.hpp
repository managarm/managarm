#pragma once

#include <stdint.h>

enum class Csr : uint16_t {
	stvec = 0x105, // Trap vector address.
	// Trap handling.
	sscratch = 0x140,
	sepc = 0x141, // Exception program counter.
	scause = 0x142, // Cause of exception.
	stval = 0x143, // Trap value.
};

// The CSR manipulation instructions on RISC-V take the CSR as an immediate operand.
// Since we do not want to add separate read/write functions for each CSR,
// using a template is out best option to ensure that the CSR is statically known.

template<Csr csr>
uint64_t readCsr() {
	uint64_t v;
	asm volatile ("csrr %0, %1" : "=r"(v) : "i"(static_cast<uint16_t>(csr)));
	return v;
}

template<Csr csr>
void writeCsr(uint64_t v) {
	asm volatile ("csrw %1, %0" : : "r"(v), "i"(static_cast<uint16_t>(csr)));
}
