#include <riscv/csr.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/timer.hpp>

extern "C" [[noreturn]] void thorRestoreExecutorRegs(void *frame);

namespace thor {

// TODO: The following functions should be moved to more appropriate places.

void restoreExecutor(Executor *executor) {
	// Check whether we return to user-mode or kernel mode.
	if (executor->general()->umode) {
		riscv::clearCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sppBit);

		auto kernelTp = reinterpret_cast<uintptr_t>(getCpuData());
		riscv::writeCsr<riscv::Csr::sscratch>(kernelTp);
	} else {
		riscv::setCsrBits<riscv::Csr::sstatus>(riscv::sstatus::sppBit);

		riscv::writeCsr<riscv::Csr::sscratch>(0);
	}

	// Disable interrupts on return.
	riscv::clearCsrBits<riscv::Csr::sstatus>(riscv::sstatus::spieBit);

	// Store return PC in sepc (since sret restores it from there).
	auto sepc = executor->general()->ip;
	riscv::writeCsr<riscv::Csr::sepc>(sepc);

	// TODO: If we restore to user mode, we also need to restore tp.

	thorRestoreExecutorRegs(executor->general());
}

extern "C" int doCopyFromUser(void *dest, const void *src, size_t size) { unimplementedOnRiscv(); }
extern "C" int doCopyToUser(void *dest, const void *src, size_t size) { unimplementedOnRiscv(); }

uint64_t getRawTimestampCounter() {
	uint64_t v;
	asm volatile("rdtime %0" : "=r"(v));
	return v;
}

// TODO: Hardwire this to true for now. The generic thor codes needs timer to be available.
bool haveTimer() { return true; }

// TODO: Implement these functions correctly:
bool preemptionIsArmed() { return false; }
void armPreemption(uint64_t nanos) { (void)nanos; }
void disarmPreemption() { unimplementedOnRiscv(); }

} // namespace thor
