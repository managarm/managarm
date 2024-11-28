#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

extern "C" [[noreturn]] void thorRestoreExecutorRegs(void *frame);

// TODO: Move declaration to header.
void handlePreemption(IrqImageAccessor image);
void handlePageFault(FaultImageAccessor image, uintptr_t address, Word errorCode);
void handleOtherFault(FaultImageAccessor image, Interrupt fault);
void handleSyscall(SyscallImageAccessor image);

namespace {

constexpr bool logTrapStubs = false;

constexpr uint64_t causeInt = UINT64_C(1) << 63;
constexpr uint64_t causeCodeMask = ((UINT64_C(1) << 63) - 1);

constexpr uint64_t codeInstructionMisaligned = 0;
constexpr uint64_t codeInstructionAccessFault = 1;
constexpr uint64_t codeIllegalInstruction = 2;
constexpr uint64_t codeBreakpoint = 3;
constexpr uint64_t codeLoadMisaligned = 4;
constexpr uint64_t codeLoadAccessFault = 5;
constexpr uint64_t codeStoreMisaligned = 6;
constexpr uint64_t codeStoreAccessFault = 7;
constexpr uint64_t codeEcallUmode = 8;
constexpr uint64_t codeInstructionPageFault = 12;
constexpr uint64_t codeLoadPageFault = 13;
constexpr uint64_t codeStorePageFault = 15;

constexpr uint64_t sstatusMask = riscv::sstatus::spieBit | riscv::sstatus::sppBit;

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
		// TODO: We need to distinguish higher half and lower half when we implement Svade.
	}

	// Note: We never set kPfAccess, but the generic code also does not rely on it.
	//       Likewise, we never set kPfBadTable.
	Word pfFlags = codeToPageFaultFlags(code);
	if (frame->umode())
		pfFlags |= kPfUser;

	handlePageFault(FaultImageAccessor{frame}, address, pfFlags);
	asm volatile("sfence.vma" : : : "memory"); // TODO: This is way too coarse.
}

void handleRiscvInterrupt(Frame *frame, uint64_t code) {
	if (logTrapStubs)
		infoLogger() << "thor: IRQ " << code << frg::endlog;

	if (code == riscv::interrupts::ssi) {
		riscv::clearCsrBits<riscv::Csr::sip>(UINT64_C(1) << riscv::interrupts::ssi);
		handlePreemption(IrqImageAccessor{frame});
	} else {
		thor::panicLogger() << "thor: Unexpected interrupt " << code << " was raised"
		                    << frg::endlog;
	}
}

void handleRiscvException(Frame *frame, uint64_t code) {
	auto trapValue = riscv::readCsr<riscv::Csr::stval>();

	const char *string = "unknown";
	if (code <= 19)
		string = exceptionStrings[code];

	if (logTrapStubs) {
		infoLogger() << "thor: Exception with code " << code << " (" << string << ")"
		             << ", trap value 0x" << frg::hex_fmt{trapValue} << " at IP 0x"
		             << frg::hex_fmt{frame->ip} << frg::endlog;

		infoLogger() << "SPP was: " << static_cast<bool>(frame->sstatus & riscv::sstatus::sppBit)
		             << ", SPIE was: "
		             << static_cast<bool>(frame->sstatus & riscv::sstatus::spieBit) << frg::endlog;

		infoLogger() << "ra: 0x" << frg::hex_fmt{frame->ra()} << ", sp: 0x"
		             << frg::hex_fmt{frame->sp()} << frg::endlog;
	}

	switch (code) {
	case codeEcallUmode:
		// We need to skip over the ecall instruction (since sepc points to ecall on entry).
		frame->ip += 4;

		handleRiscvSyscall(frame);
		break;
	case codeInstructionPageFault:
		[[fallthrough]];
	case codeLoadPageFault:
	case codeStorePageFault:
		handleRiscvPageFault(frame, code, trapValue);
		break;
	case codeIllegalInstruction:
		handleOtherFault(FaultImageAccessor{frame}, kIntrIllegalInstruction);
		break;
	case codeBreakpoint:
		handleOtherFault(FaultImageAccessor{frame}, kIntrBreakpoint);
		break;
	case codeInstructionMisaligned:
	case codeInstructionAccessFault:
	case codeLoadMisaligned:
	case codeLoadAccessFault:
	case codeStoreMisaligned:
	case codeStoreAccessFault:
		infoLogger() << "Exception with code " << code << ", trap value 0x"
		             << frg::hex_fmt{trapValue} << " at IP 0x" << frg::hex_fmt{frame->ip}
		             << frg::endlog;
		handleOtherFault(FaultImageAccessor{frame}, kIntrGeneralFault);
		break;
	default:
		panicLogger() << "Unexpected exception with code " << code << ", trap value 0x"
		              << frg::hex_fmt{trapValue} << " at IP 0x" << frg::hex_fmt{frame->ip}
		              << frg::endlog;
	}
}

void writeSretCsrs(Frame *frame) {
	auto sstatusForExit =
	    (riscv::readCsr<riscv::Csr::sstatus>() & ~sstatusMask) | (frame->sstatus & sstatusMask);
	if (frame->umode()) {
		auto kernelTp = reinterpret_cast<uintptr_t>(getCpuData());
		riscv::writeCsr<riscv::Csr::sscratch>(kernelTp);
	}
	riscv::writeCsr<riscv::Csr::sstatus>(sstatusForExit);
	riscv::writeCsr<riscv::Csr::sepc>(frame->ip);
}

} // namespace

extern "C" void thorHandleException(Frame *frame) {
	// Perform the trap entry.
	frame->sstatus = riscv::readCsr<riscv::Csr::sstatus>();
	auto cause = riscv::readCsr<riscv::Csr::scause>();

	// Call the actual IRQ or exception handler.
	auto code = cause & causeCodeMask;
	if (cause & causeInt) {
		handleRiscvInterrupt(frame, code);
	} else {
		handleRiscvException(frame, code);
	}

	// Now perform the trap exit.
	writeSretCsrs(frame);
}

void restoreExecutor(Executor *executor) {
	getCpuData()->exceptionStackPtr = executor->_exceptionStack;

	writeSretCsrs(executor->general());
	// TODO: In principle, this is only necessary on CPU migration.
	if (!executor->general()->umode())
		executor->general()->tp() = reinterpret_cast<uintptr_t>(getCpuData());
	thorRestoreExecutorRegs(executor->general());
}

} // namespace thor
