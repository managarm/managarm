#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/trap.hpp>

namespace thor {

// TODO: Move declaration to header.
void handlePageFault(FaultImageAccessor image, uintptr_t address, Word errorCode);

namespace {

static constexpr uint64_t codeInstructionPageFault = 12;
static constexpr uint64_t codeLoadPageFault = 13;
static constexpr uint64_t codeStorePageFault = 15;

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

} // namespace

extern "C" void thorHandleException(Frame *frame) {
	auto cause = riscv::readCsr<riscv::Csr::scause>();
	auto code = cause & ((UINT64_C(1) << 63) - 1);
	if (cause & (UINT64_C(1) << 63)) {
		infoLogger() << "thor: IRQ" << frg::endlog;
	} else {
		auto status = riscv::readCsr<riscv::Csr::sstatus>();
		auto ip = riscv::readCsr<riscv::Csr::sepc>();
		auto trapValue = riscv::readCsr<riscv::Csr::stval>();

		frame->umode = !(status & riscv::sstatus::sppBit);

		const char *string = "unknown";
		if (code <= 19)
			string = exceptionStrings[code];

		infoLogger() << "thor: Exception with code " << code << " (" << string << ")"
		             << ", trap value 0x" << frg::hex_fmt{trapValue} << " at IP 0x"
		             << frg::hex_fmt{ip} << frg::endlog;

		infoLogger() << "SPP was: " << static_cast<bool>(status & riscv::sstatus::sppBit)
		             << ", SPIE was: " << static_cast<bool>(status & riscv::sstatus::spieBit)
		             << frg::endlog;

		infoLogger() << "ra: 0x" << frg::hex_fmt{frame->ra()} << ", sp: 0x"
		             << frg::hex_fmt{frame->sp()} << frg::endlog;

		if (code == codeInstructionPageFault || code == codeLoadPageFault ||
		    code == codeStorePageFault) {
			return handleRiscvPageFault(frame, code, trapValue);
		}
	}
	while (true)
		;
}

} // namespace thor
