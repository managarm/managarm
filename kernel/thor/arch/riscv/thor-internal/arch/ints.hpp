#pragma once

#include <assert.h>
#include <frg/spinlock.hpp>
#include <riscv/csr.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/debug.hpp>

namespace thor {

void initializeIrqVectors();

inline bool intsAreEnabled() {
	return riscv::readCsr<riscv::Csr::sstatus>() & riscv::sstatus::sieBit;
}

inline void enableInts() { return riscv::setCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sieBit); }

inline void disableInts() {
	return riscv::clearCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sieBit);
}

inline void halt() { asm volatile("wfi"); }

void suspendSelf();

} // namespace thor
