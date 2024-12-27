#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/fp-state.hpp>
#include <thor-internal/arch/timer.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/thread.hpp>

namespace thor {

extern "C" [[noreturn]] void thorRestoreExecutorRegs(void *frame);

// TODO: Move declaration to header.
void handlePreemption(IrqImageAccessor image);
void handleIrq(IrqImageAccessor image, IrqPin *irq);
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

// Bits of sstatus that we save/restore on context switch.
constexpr uint64_t sstatusMask = riscv::sstatus::spieBit | riscv::sstatus::sppBit
                                 | (riscv::sstatus::extMask << riscv::sstatus::fsShift);

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

// Modifies frame->sstatus. Must be called *before* sstatus is restored.
void restoreStaleExtendedState(Executor *executor, Frame *frame) {
	auto cpuData = getCpuData();

	// Load floating point state.
	auto fs = (frame->sstatus >> riscv::sstatus::fsShift) & riscv::sstatus::extMask;
	if (fs) {
		if (!cpuData->stashedFs) {
			// Note that we have to enable the FP extension first since we disable it in the kernel.
			// extDirty is all-ones hence the setCsrBits() is enough.
			riscv::setCsrBits<riscv::Csr::sstatus>(
			    riscv::sstatus::extDirty << riscv::sstatus::fsShift
			);

			auto fs = reinterpret_cast<uint64_t *>(executor->fpRegisters());
			riscv::writeCsr<riscv::Csr::fcsr>(fs[32]);
			restoreFpRegisters(fs);

			// sstatus is later reloaded from the frame. Mark FS as clean.
			frame->sstatus &= ~(riscv::sstatus::extMask << riscv::sstatus::fsShift);
			frame->sstatus |= riscv::sstatus::extClean << riscv::sstatus::fsShift;
		} else {
			assert(fs == cpuData->stashedFs);
		}
		cpuData->stashedFs = 0;
	}
}

void handleRiscvIpi(Frame *frame) {
	auto *cpuData = getCpuData();

	// Clear the IPI.This must happen before clearing pendingIpis,
	// otherwise we could lose IPIs that become pending concurrently.
	riscv::clearCsrBits<riscv::Csr::sip>(UINT64_C(1) << riscv::interrupts::ssi);

	// Read the bitmask of pending IPIs and process all of them.
	auto mask = cpuData->pendingIpis.exchange(0, std::memory_order_acq_rel);

	if (mask & PlatformCpuData::ipiShootdown) {
		for (auto &binding : getCpuData()->asidData->bindings)
			binding.shootdown();

		getCpuData()->asidData->globalBinding.shootdown();
	}

	if (mask & PlatformCpuData::ipiSelfCall)
		SelfIntCallBase::runScheduledCalls();

	// Note: since the following code can re-schedule and discard the current call chain,
	//       we *must* handle ping IPIs last.
	if (mask & PlatformCpuData::ipiPing)
		handlePreemption(IrqImageAccessor{frame});
}

void handleRiscvSyscall(Frame *frame) { handleSyscall(SyscallImageAccessor{frame}); }

void handleRiscvPageFault(Frame *frame, uint64_t code, uint64_t address) {
	if (!inHigherHalf(address)) {
		// updatePageAccess() on RISC-V always sets the A bit, no need to set extra flags.
		PageFlags flags = 0;
		if (code == codeInstructionPageFault) {
			flags |= page_access::execute;
		} else if (code == codeLoadPageFault) {
			flags |= page_access::read;
		} else if (code == codeStorePageFault) {
			flags |= page_access::write;
		}

		auto thisThread = getCurrentThread();
		auto addressSpace = thisThread->getAddressSpace();
		if (addressSpace->updatePageAccess(address & ~(kPageSize - 1), flags)) {
			// infoLogger() << "updateSuccess" << frg::endlog;
			asm volatile("sfence.vma" : : : "memory"); // TODO: This is way too coarse.
			return;
		}
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
		handleRiscvIpi(frame);
	} else if (code == riscv::interrupts::sti) {
		onTimerInterrupt(IrqImageAccessor{frame});
	} else if (code == riscv::interrupts::sei) {
		auto irq = claimExternalIrq();
		if (irq) {
			handleIrq(IrqImageAccessor{frame}, irq);
		} else {
			thor::infoLogger() << "Spurious external interrupt" << frg::endlog;
		}
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
			infoLogger() << "thor: Exception with code " << code << ", trap value 0x"
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
	auto cpuData = getCpuData();

	// Perform the trap entry.
	frame->sstatus = riscv::readCsr<riscv::Csr::sstatus>();
	// TODO: This could be combined with the CSR read above.
	riscv::clearCsrBits<riscv::Csr::sstatus>((riscv::sstatus::extMask << riscv::sstatus::fsShift));
	auto cause = riscv::readCsr<riscv::Csr::scause>();

	// Disable FP.
	auto fs = (frame->sstatus >> riscv::sstatus::fsShift) & riscv::sstatus::extMask;
	if (fs) {
		assert(!cpuData->stashedFs);
		cpuData->stashedFs = fs;
	}

	// Call the actual IRQ or exception handler.
	auto code = cause & causeCodeMask;
	if (cause & causeInt) {
		handleRiscvInterrupt(frame, code);
	} else {
		handleRiscvException(frame, code);
	}
	assert(!intsAreEnabled());
	assert(cpuData == getCpuData());

	// Now perform the trap exit.
	restoreStaleExtendedState(cpuData->activeExecutor, frame);
	writeSretCsrs(frame);
}

void restoreExecutor(Executor *executor) {
	auto *cpuData = getCpuData();
	auto *frame = executor->general();

	// TODO: This should probably be done in some activateExectutor() function.
	cpuData->activeExecutor = executor;
	cpuData->exceptionStackPtr = executor->_exceptionStack;

	assert(!cpuData->stashedFs);
	restoreStaleExtendedState(executor, frame);
	writeSretCsrs(frame);
	// TODO: In principle, this is only necessary on CPU migration.
	if (!frame->umode())
		frame->tp() = reinterpret_cast<uintptr_t>(cpuData);
	thorRestoreExecutorRegs(frame);
}

void handleRiscvWorkOnExecutor(Executor *executor, Frame *frame) {
	auto *cpuData = getCpuData();

	enableInts();
	getCurrentThread()->mainWorkQueue()->run();
	disableInts();

	assert(!cpuData->stashedFs);
	restoreStaleExtendedState(executor, frame);
	writeSretCsrs(frame);
	// TODO: In principle, this is only necessary on CPU migration.
	if (!frame->umode())
		frame->tp() = reinterpret_cast<uintptr_t>(cpuData);
	thorRestoreExecutorRegs(frame);
}

} // namespace thor
