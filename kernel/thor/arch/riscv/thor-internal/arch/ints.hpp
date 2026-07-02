#pragma once

#include <riscv/csr.hpp>

namespace thor {

struct CpuData;

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
void sendHypervisorIpi(CpuData *dstData);

} // namespace thor
