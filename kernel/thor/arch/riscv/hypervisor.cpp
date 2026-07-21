#include <riscv/csr.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/fp-state.hpp>
#include <thor-internal/arch/hypervisor.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/load-balancing.hpp>
#include <thor-internal/thread.hpp>

extern "C" void riscvHypervisorRun(HelRiscv64VirtualizationRegs *regs);
extern "C" void riscvHypervisorRestore();

extern "C" bool riscvHypervisorInstRead(uint32_t *dest, uintptr_t guestAddress, uint64_t *scause);
extern "C" void riscvHypervisorLoadFaultStub();

namespace thor::riscv_hypervisor {

namespace {

uint64_t hgatpPagingMode = 0;

} // namespace

void init() {
	uint64_t hstatus = riscv::readCsr<riscv::Csr::hstatus>();
	// Little-endian VS.
	hstatus &= ~riscv::hstatus::vsbe;
	hstatus &= ~riscv::hstatus::spv;
	hstatus &= ~riscv::hstatus::hu;
	hstatus &= ~riscv::hstatus::vtvm;
	// Trap wfi.
	hstatus |= riscv::hstatus::vtw;
	hstatus &= ~riscv::hstatus::vtsr;

	// 64-bit
	hstatus |= UINT64_C(2) << riscv::hstatus::vsxlShift;
	// Disable pointer masking.
	hstatus &= ~(riscv::hstatus::hupmmMask << riscv::hstatus::hupmmShift);

	riscv::writeCsr<riscv::Csr::hstatus>(hstatus);

	// Delegate VSEI/VSTI/VSSI to the guest.
	riscv::writeCsr<riscv::Csr::hideleg>((1 << 10) | (1 << 6) | (1 << 2));
	// Delegate all guaranteed exceptions to the guest.
	riscv::writeCsr<riscv::Csr::hedeleg>(0xcb1ff);

	// Try to pass through cycle/time/instret counters.
	uint64_t counters = riscv::hcounteren::cy | riscv::hcounteren::tm | riscv::hcounteren::ir;
	riscv::setCsrBits<riscv::Csr::hcounteren>(counters);

	uint64_t henvcfg = riscv::readCsr<riscv::Csr::henvcfg>();
	// Disable branch tracking.
	henvcfg &= ~riscv::henvcfg::lpe;
	// Disable shadow stack.
	henvcfg &= ~riscv::henvcfg::sse;

	henvcfg &= ~(riscv::henvcfg::cbieMask << riscv::henvcfg::cbieShift);

	// Enable cache instructions.
	if (riscvHartCapsNote->hasExtension(RiscvExtension::zicbom)) {
		henvcfg |= riscv::henvcfg::cbcfe;
		henvcfg |= (riscv::henvcfg::cbieGuestControl << riscv::henvcfg::cbieShift);
	}
	if (riscvHartCapsNote->hasExtension(RiscvExtension::zicboz))
		henvcfg |= riscv::henvcfg::cbze;
	else
		henvcfg &= ~riscv::henvcfg::cbze;

	// Enable double trap.
	if (riscvHartCapsNote->hasExtension(RiscvExtension::ssdbltrp))
		henvcfg |= riscv::henvcfg::dte;
	else
		henvcfg &= ~riscv::henvcfg::dte;

	// Enable PTE A/D bit updating.
	if (riscvHartCapsNote->hasExtension(RiscvExtension::svadu))
		henvcfg |= riscv::henvcfg::adue;
	else
		henvcfg &= ~riscv::henvcfg::adue;

	// Enable page-based memory types.
	if (riscvHartCapsNote->hasExtension(RiscvExtension::svpbmt))
		henvcfg |= riscv::henvcfg::pbmte;
	else
		henvcfg &= ~riscv::henvcfg::pbmte;

	// Enable stimecmp.
	if (riscvHartCapsNote->hasExtension(RiscvExtension::sstc))
		henvcfg |= riscv::henvcfg::stce;
	else
		henvcfg &= ~riscv::henvcfg::stce;

	// Disable pointer masking.
	henvcfg &= ~(riscv::henvcfg::pmmMask << riscv::henvcfg::pmmShift);

	riscv::writeCsr<riscv::Csr::henvcfg>(henvcfg);

	if (riscvConfigNote->numPtLevels == 3)
		hgatpPagingMode = riscv::hgatp::sv39;
	else if (riscvConfigNote->numPtLevels == 4)
		hgatpPagingMode = riscv::hgatp::sv48;
	else {
		assert(riscvConfigNote->numPtLevels == 5);
		hgatpPagingMode = riscv::hgatp::sv57;
	}
}

Vcpu::Vcpu(CtorToken, smarter::shared_ptr<HypervisorSpace> space) : space_{std::move(space)} {
	state_.kernelMode = true;
}

Vcpu::~Vcpu() {}

frg::expected<Error, HelVmexitReason> Vcpu::run() {
	while (true) {
		if (getCurrentThread()->checkCancelConditions())
			return Error::cancelled;

		Thread::asyncBlockCurrent(mutex_.async_lock(), getCurrentThread()->mainWorkQueue().get());
		frg::unique_lock lock{frg::adopt_lock, mutex_};

		{
			auto irqLock = frg::guard(&irqMutex());

			uint64_t hgatp =
			    (hgatpPagingMode << riscv::hgatp::modeShift) | (space_->rootTable() >> kPageShift);
			riscv::writeCsr<riscv::Csr::hgatp>(hgatp);

			// Invalidate the translations, loading hgatp doesn't automatically invalidate anything.
			// TODO: This could be VMID-scoped when VMID support is added.
			asm volatile("hfence.gvma");

			auto sstatus = riscv::readCsr<riscv::Csr::sstatus>();
			// Enable the FP extension since we disable it in the kernel.
			riscv::writeCsr<riscv::Csr::sstatus>(
			    sstatus | riscv::sstatus::extDirty << riscv::sstatus::fsShift
			);

			// Save host FPU state.
			alignas(8) char fpSave[Executor::fpStateSize];
			saveFpRegisters(fpSave);
			uint64_t fcsr = riscv::readCsr<riscv::Csr::fcsr>();

			// Restore guest FPU state.
			riscv::writeCsr<riscv::Csr::fcsr>(
			    *reinterpret_cast<uint64_t *>(fpState_ + sizeof(fpState_) - 8)
			);
			restoreFpRegisters(fpState_);

			{
				frg::unique_lock irqInjectionLock{irqInjectionMutex_};
				runningCpu_ = getCpuData();
				// This is done under the irq injection mutex to prevent hvip from being modified.
				restoreHypervisorCsrs_();
			}

			riscvHypervisorRun(&state_);

			{
				frg::unique_lock irqInjectionLock{irqInjectionMutex_};
				runningCpu_ = nullptr;
			}

			saveHypervisorCsrs_();

			// Save guest FPU state.
			saveFpRegisters(fpState_);
			uint64_t guestFcsr = riscv::readCsr<riscv::Csr::fcsr>();
			*reinterpret_cast<uint64_t *>(fpState_ + sizeof(fpState_) - 8) = guestFcsr;

			// Restore host FPU state.
			riscv::writeCsr<riscv::Csr::fcsr>(fcsr);
			restoreFpRegisters(fpSave);
			riscv::writeCsr<riscv::Csr::sstatus>(sstatus);
		}

		HelVmexitReason exitReason{};
		if (handleException_(exitReason))
			return exitReason;
	}

	return {};
}

void Vcpu::restoreHypervisorCsrs_() {
	riscv::writeCsr<riscv::Csr::vsstatus>(state_.sstatus);
	riscv::writeCsr<riscv::Csr::vsie>(state_.sie);
	riscv::writeCsr<riscv::Csr::vstvec>(state_.stvec);
	riscv::writeCsr<riscv::Csr::vsscratch>(state_.sscratch);
	riscv::writeCsr<riscv::Csr::vsepc>(state_.sepc);
	riscv::writeCsr<riscv::Csr::vscause>(state_.scause);
	riscv::writeCsr<riscv::Csr::vstval>(state_.stval);
	riscv::writeCsr<riscv::Csr::vsip>(state_.sip);
	riscv::writeCsr<riscv::Csr::vsatp>(state_.satp);

	if (riscvHartCapsNote->hasExtension(RiscvExtension::sstc))
		riscv::writeCsr<riscv::Csr::vstimecmp>(state_.stimecmp);

	riscv::writeCsr<riscv::Csr::sepc>(state_.pc);

	// Setup sstatus/hstatus for returning to the appropriate mode.

	uint64_t sstatus = riscv::readCsr<riscv::Csr::sstatus>();
	uint64_t hstatus = riscv::readCsr<riscv::Csr::hstatus>();

	// Enable host interrupts.
	sstatus |= riscv::sstatus::spieBit;
	// Enable virtualization mode.
	hstatus |= riscv::hstatus::spv;

	if (state_.kernelMode) {
		sstatus |= riscv::sstatus::sppBit;
		hstatus |= riscv::hstatus::spvp;
	} else {
		sstatus &= ~riscv::sstatus::sppBit;
		hstatus &= ~riscv::hstatus::spvp;
	}

	riscv::writeCsr<riscv::Csr::sstatus>(sstatus);
	riscv::writeCsr<riscv::Csr::hstatus>(hstatus);

	riscv::writeCsr<riscv::Csr::hvip>(hvip_);

	// Set stvec to hypervisor restore.
	auto stvec = reinterpret_cast<uint64_t>(riscvHypervisorRestore);
	assert(!(stvec & 3));
	riscv::writeCsr<riscv::Csr::stvec>(stvec);
}

void Vcpu::saveHypervisorCsrs_() {
	state_.sstatus = riscv::readCsr<riscv::Csr::vsstatus>();
	state_.sie = riscv::readCsr<riscv::Csr::vsie>();
	state_.stvec = riscv::readCsr<riscv::Csr::vstvec>();
	state_.sscratch = riscv::readCsr<riscv::Csr::vsscratch>();
	state_.sepc = riscv::readCsr<riscv::Csr::vsepc>();
	state_.scause = riscv::readCsr<riscv::Csr::vscause>();
	state_.stval = riscv::readCsr<riscv::Csr::vstval>();
	state_.sip = riscv::readCsr<riscv::Csr::vsip>();
	state_.satp = riscv::readCsr<riscv::Csr::vsatp>();

	if (riscvHartCapsNote->hasExtension(RiscvExtension::sstc))
		state_.stimecmp = riscv::readCsr<riscv::Csr::vstimecmp>();

	state_.pc = riscv::readCsr<riscv::Csr::sepc>();

	scause_ = riscv::readCsr<riscv::Csr::scause>();
	stval_ = riscv::readCsr<riscv::Csr::stval>();
	htval_ = riscv::readCsr<riscv::Csr::htval>();
	htinst_ = riscv::readCsr<riscv::Csr::htinst>();

	riscv::clearCsrBits<riscv::Csr::hstatus>(riscv::hstatus::spv);

	auto hstatus = riscv::readCsr<riscv::Csr::hstatus>();
	state_.kernelMode = (hstatus & riscv::hstatus::spvp) ? true : false;

	// Restore the old stvec.
	auto stvec = reinterpret_cast<uint64_t>(thor::thorExceptionEntry);
	assert(!(stvec & 3));
	riscv::writeCsr<riscv::Csr::stvec>(stvec);
}

namespace {

constexpr uint64_t causeInt = UINT64_C(1) << 63;
constexpr uint64_t causeCodeMask = ((UINT64_C(1) << 63) - 1);

} // namespace

bool Vcpu::handleException_(HelVmexitReason &exitReason) {
	bool interrupt = scause_ & causeInt;

	if (interrupt)
		return false;

	auto code = scause_ & causeCodeMask;

	switch (code) {
		// ecall from VS-mode
		case 10:
			exitReason.exitReason = kHelVmexitHyperCall;
			exitReason.address = state_.pc;
			return true;
		// Double trap
		case 16:
			exitReason.exitReason = kHelVmexitError;
			exitReason.address = state_.pc;
			return true;
		// Instruction guest-page fault
		case 20:
			return handlePageFault_(exitReason, AddressSpace::kFaultExecute);
		// Load guest-page fault
		case 21:
			return handlePageFault_(exitReason, {});
		// Virtual instruction
		case 22: {
			uint64_t inst = stval_;

			if (inst == 0) {
				if (auto result = readInstruction_(state_.pc)) {
					inst = *result;

					// If the instruction is compressed set the compressed flag.
					if ((*result & 0b11) != 0b11)
						inst |= UINT64_C(1) << 63;
				} else {
					code = scause_ & causeCodeMask;

					// Load guest-page fault
					if (code == 21)
						return handlePageFault_(exitReason, AddressSpace::kFaultExecute);

					// The instruction couldn't be fetched, report an error.
					exitReason.exitReason = kHelVmexitError;
					exitReason.address = state_.pc;
					return true;
				}
			}

			exitReason.exitReason = kHelVmexitInstructionTrap;
			exitReason.address = state_.pc;
			exitReason.instruction = inst;
			return true;
		}
		// Store/AMO guest-page fault
		case 23:
			return handlePageFault_(exitReason, AddressSpace::kFaultWrite);
		default:
			break;
	}

	return false;
}

bool Vcpu::handlePageFault_(HelVmexitReason &exitReason, uint32_t flags) {
	uint64_t address = (htval_ << 2) | (stval_ & 0b11);

	auto faultOutcome = Thread::asyncBlockCurrent(
	    space_->handleFault(address, flags), getCurrentThread()->pagingWorkQueue().get()
	);
	if (!faultOutcome) {
		size_t exitFlags = kHelVmFaultRead;
		if (flags & AddressSpace::kFaultWrite) {
			exitFlags = kHelVmFaultWrite;
		}
		if (flags & AddressSpace::kFaultExecute) {
			exitFlags = kHelVmFaultExecute;
		}

		if (htinst_ == 0) {
			if (auto result = readInstruction_(state_.pc)) {
				htinst_ = *result;

				// If the instruction is compressed set the compressed flag.
				if ((*result & 0b11) != 0b11)
					htinst_ |= UINT64_C(1) << 63;
			} else {
				// The instruction couldn't be fetched, report an error.
				exitReason.exitReason = kHelVmexitError;
				exitReason.address = state_.pc;
				return true;
			}
		}

		exitReason.exitReason = kHelVmexitTranslationFault;
		exitReason.instruction = htinst_;
		exitReason.address = address;
		exitReason.flags = exitFlags;

		return true;
	}

	return false;
}

std::optional<uint32_t> Vcpu::readInstruction_(uintptr_t address) {
	auto irqLock = frg::guard(&irqMutex());

	// Set stvec to hypervisor load fault stub.
	auto stvec = reinterpret_cast<uint64_t>(riscvHypervisorLoadFaultStub);
	assert(!(stvec & 3));
	riscv::writeCsr<riscv::Csr::stvec>(stvec);

	uint64_t hgatp =
	    (hgatpPagingMode << riscv::hgatp::modeShift) | (space_->rootTable() >> kPageShift);
	riscv::writeCsr<riscv::Csr::hgatp>(hgatp);
	riscv::writeCsr<riscv::Csr::vsatp>(state_.satp);

	if (state_.kernelMode)
		riscv::setCsrBits<riscv::Csr::hstatus>(riscv::hstatus::spvp);
	else
		riscv::clearCsrBits<riscv::Csr::hstatus>(riscv::hstatus::spvp);

	// Invalidate the translations, loading hgatp doesn't automatically invalidate anything.
	// TODO: This could be VMID-scoped when VMID support is added.
	asm volatile("hfence.gvma");

	enableUserAccess();

	uint32_t inst;
	std::optional<uint32_t> ret = std::nullopt;

	if (riscvHypervisorInstRead(&inst, address, &scause_))
		ret = inst;
	else {
		// Read the other fault CSRs for the new fault.
		stval_ = riscv::readCsr<riscv::Csr::stval>();
		htval_ = riscv::readCsr<riscv::Csr::htval>();
		htinst_ = riscv::readCsr<riscv::Csr::htinst>();
	}

	disableUserAccess();

	// Restore the old stvec.
	stvec = reinterpret_cast<uint64_t>(thor::thorExceptionEntry);
	assert(!(stvec & 3));
	riscv::writeCsr<riscv::Csr::stvec>(stvec);

	return ret;
}

void Vcpu::storeRegs(const HelVirtualizationRegs *regs) { memcpy(&state_, regs, sizeof(*regs)); }

void Vcpu::loadRegs(HelVirtualizationRegs *res) { memcpy(res, &state_, sizeof(*res)); }

/*
 * 2 VSSI (software irq)
 * 6 VSTI (timer irq)
 * 10 VSEI (external irq)
 */
bool Vcpu::assertInterrupt(uint64_t number, bool level) {
	if (number != 2 && number != 6 && number != 10)
		return false;

	frg::unique_lock lock{irqInjectionMutex_};

	if (level)
		hvip_ |= UINT64_C(1) << number;
	else
		hvip_ &= ~(UINT64_C(1) << number);

	if (runningCpu_)
		sendHypervisorIpi(runningCpu_);

	return true;
}

} // namespace thor::riscv_hypervisor
