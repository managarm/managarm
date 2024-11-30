#pragma once

#include <stdint.h>

namespace riscv {

enum class Csr : uint16_t {
	// Floating point control and status.
	fcsr = 0x3,
	// Supervisor trap setup.
	sstatus = 0x100, // Status register.
	sie = 0x104,     // Interrupt enable.
	stvec = 0x105,   // Trap vector address.
	// Supervisor trap handling.
	sscratch = 0x140,
	sepc = 0x141,   // Exception program counter.
	scause = 0x142, // Cause of exception.
	stval = 0x143,  // Trap value.
	sip = 0x0144,   // Interrupt pending.
	// Supervisor protection and translation.
	satp = 0x180, // Address translation.
};

namespace sstatus {

constexpr uint64_t sieBit = UINT64_C(1) << 1;  // Interrupt enable.
constexpr uint64_t spieBit = UINT64_C(1) << 5; // Previous interrupt enable.
constexpr uint64_t ubeBit = UINT64_C(1) << 6;  // U-mode is big endian or not.
constexpr uint64_t sppBit = UINT64_C(1) << 8;  // Previous privilege level (S-mode or not).
constexpr uint64_t sumBit = UINT64_C(1) << 18; // User memory access permitted in S-mode.
constexpr uint64_t mxrBit = UINT64_C(1)
                            << 19; // Executable page permission implies readable in S-mode.

// VS, FS, XS: floating point and vector extension state.
constexpr int vsShift = 9;
constexpr int fsShift = 13;
constexpr int xsShift = 15;

// Values for the VS, FS, XS fields.
constexpr uint64_t extMask = 3;
constexpr uint64_t extOff = 0;
constexpr uint64_t extInitial = 1;
constexpr uint64_t extClean = 2;
constexpr uint64_t extDirty = 3;

// U-mode execution width.
constexpr int uxlShift = 32;
constexpr uint64_t uxlMask = 3;
constexpr uint64_t uxl32 = 1;
constexpr uint64_t uxl64 = 2;
constexpr uint64_t uxl128 = 3;

} // namespace sstatus

namespace interrupts {

constexpr uint64_t ssi = 1; // Supervisor software interrupt.
constexpr uint64_t sti = 5; // Supervisor timer interrupt.
constexpr uint64_t sei = 9; // Supervisor external interrupt.

} // namespace interrupts

// The CSR manipulation instructions on RISC-V take the CSR as an immediate operand.
// Since we do not want to add separate read/write functions for each CSR,
// using a template is out best option to ensure that the CSR is statically known.

template <Csr csr>
uint64_t readCsr() {
	uint64_t v;
	asm volatile("csrr %0, %1" : "=r"(v) : "i"(static_cast<uint16_t>(csr)) : "memory");
	return v;
}

template <Csr csr>
void writeCsr(uint64_t v) {
	asm volatile("csrw %1, %0" : : "r"(v), "i"(static_cast<uint16_t>(csr)) : "memory");
}

template <Csr csr>
void setCsrBits(uint64_t v) {
	asm volatile("csrs %1, %0" : : "r"(v), "i"(static_cast<uint16_t>(csr)) : "memory");
}

template <Csr csr>
void clearCsrBits(uint64_t v) {
	asm volatile("csrc %1, %0" : : "r"(v), "i"(static_cast<uint16_t>(csr)) : "memory");
}

} // namespace riscv
