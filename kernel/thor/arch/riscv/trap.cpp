#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/trap.hpp>

namespace thor {

// TODO: Move declaration to header.
void handlePageFault(FaultImageAccessor image, uintptr_t address, Word errorCode);
void handleSyscall(SyscallImageAccessor image);

namespace {

constexpr uint64_t causeInt = UINT64_C(1) << 63;
constexpr uint64_t causeCodeMask = ((UINT64_C(1) << 63) - 1);

constexpr uint64_t codeEcallUmode = 8;
constexpr uint64_t codeInstructionPageFault = 12;
constexpr uint64_t codeLoadPageFault = 13;
constexpr uint64_t codeStorePageFault = 15;

constexpr const char *exceptionStrings[] = {
    "instruction misaligned",
    "instruction access fault",
    "illegal instruction",
    "breakpoint",
    "load misaligned",
    "load access fault",
    "store misaligned",
    "store access fault",
    "u-mode ecall",
    "s-mode ecall",
    nullptr,
    nullptr,
    "instruction page fault",
    "load page fault",
    nullptr,
    "store page fault",
    nullptr,
    nullptr,
    "software check",
    "hardware error",
};

Word codeToPageFaultFlags(uint64_t code) {
	if (code == codeInstructionPageFault)
		return kPfInstruction;
	if (code == codeStorePageFault)
		return kPfWrite;
	assert(code == codeLoadPageFault);
	return 0;
}

void handleRiscvSyscall(Frame *frame) { handleSyscall(SyscallImageAccessor{frame}); }

void handleRiscvPageFault(Frame *frame, uint64_t code, uint64_t address) {
	if (!inHigherHalf(address)) {
		// Note: We never set kPfAccess, but the generic code also does not rely on it.
		//       Likewise, we never set kPfBadTable.
		Word pfFlags = codeToPageFaultFlags(code);
		if (frame->umode)
			pfFlags |= kPfUser;

		handlePageFault(FaultImageAccessor{frame}, address, pfFlags);
		asm volatile("sfence.vma" : : : "memory"); // TODO: This is way too coarse.
	} else {
		// TODO: Implement page faults in higher half pages.
		unimplementedOnRiscv();
	}
}

void handleRiscvIrq(Frame *frame, uint64_t code) { infoLogger() << "thor: IRQ" << frg::endlog; }

void handleRiscvException(Frame *frame, uint64_t code, uint64_t status) {
	infoLogger() << "a0 is 0x" << frg::hex_fmt{frame->a(0)} << frg::endlog;
	auto trapValue = riscv::readCsr<riscv::Csr::stval>();

	const char *string = "unknown";
	if (code <= 19)
		string = exceptionStrings[code];

	infoLogger() << "thor: Exception with code " << code << " (" << string << ")"
	             << ", trap value 0x" << frg::hex_fmt{trapValue} << " at IP 0x"
	             << frg::hex_fmt{frame->ip} << frg::endlog;

	infoLogger() << "SPP was: " << static_cast<bool>(status & riscv::sstatus::sppBit)
	             << ", SPIE was: " << static_cast<bool>(status & riscv::sstatus::spieBit)
	             << frg::endlog;

	infoLogger() << "ra: 0x" << frg::hex_fmt{frame->ra()} << ", sp: 0x" << frg::hex_fmt{frame->sp()}
	             << frg::endlog;

	if (code == codeEcallUmode) {
		// We need to skip over the ecall instruction (since sepc points to ecall on entry).
		frame->ip += 4;

		handleRiscvSyscall(frame);
	} else if (code == codeInstructionPageFault || code == codeLoadPageFault ||
	           code == codeStorePageFault) {
		handleRiscvPageFault(frame, code, trapValue);
	} else {
		panicLogger() << "Unexpected exception" << frg::endlog;
	}
}

} // namespace

extern "C" void thorHandleException(Frame *frame) {
	auto status = riscv::readCsr<riscv::Csr::sstatus>();
	frame->umode = !(status & riscv::sstatus::sppBit);

	// Call the actual IRQ or exception handler.
	auto cause = riscv::readCsr<riscv::Csr::scause>();
	auto code = cause & causeCodeMask;
	if (cause & causeInt) {
		handleRiscvIrq(frame, code);
	} else {
		handleRiscvException(frame, code, status);
	}

	if (frame->umode) {
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
	riscv::writeCsr<riscv::Csr::sepc>(frame->ip);
}

} // namespace thor
