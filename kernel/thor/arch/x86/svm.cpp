#include <optional>

#include <thor-internal/arch/svm.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>
#include <x86/machine.hpp>

extern "C" void svmVmRun(thor::svm::GprState *gprs, PhysicalAddr vmcb);

namespace {

constexpr uint64_t EFER_SVME = 1 << 12;
constexpr uint64_t CR0_PE = 1 << 0;
constexpr uint64_t CR4_OSXSAVE = 1 << 18;

constexpr uint8_t DB_VECTOR = 1;
constexpr uint8_t AC_VECTOR = 17;
constexpr uint8_t MC_VECTOR = 18;

} // namespace

namespace thor::svm {
	bool init() {
		if(!getGlobalCpuFeatures()->haveSvm)
			return false;

		infoLogger() << "svm: Enabling SVM" << frg::endlog;

		auto efer = common::x86::rdmsr(common::x86::kMsrEfer);
		common::x86::wrmsr(common::x86::kMsrEfer, efer | EFER_SVME);

		auto hsaveRegion = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
		common::x86::wrmsr(0xC0010117, hsaveRegion);

		return true;
	}

	Vcpu::Vcpu(CtorToken, smarter::shared_ptr<NptSpace> npt) : space{npt} {
		vmcb_region = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
		host_additional_save_region = physicalAllocator->allocateInfallible(PhysicalKind::pinned);
		PageAccessor regionAccessor{vmcb_region};
		vmcb = (volatile Vmcb *)regionAccessor.get();

		memset((void *)vmcb, 0, kPageSize);

		// The guest can make a CPU deliver #DB/#AC in an endless loop that blocks host interrupts,
		// intercepting them breaks that loop (CVE-2015-8104, CVE-2015-5307).
		// TODO: Handle machine checks in the host.
		vmcb->iceptExceptions = (1 << DB_VECTOR) | (1 << AC_VECTOR) | (1 << MC_VECTOR);

		vmcb->iceptVmrun = 1;
		vmcb->iceptVmmcall = 1;
		vmcb->iceptVmload = 1;
		vmcb->iceptVmsave = 1;
		vmcb->iceptStgi = 1;
		vmcb->iceptClgi = 1;

		vmcb->iceptIntr = 1;
		vmcb->iceptNmi = 1;
		vmcb->iceptSmi = 1;

		vmcb->iceptHlt = 1;
		vmcb->iceptCpuid = 1;
		vmcb->iceptRdpmc = 1;
		vmcb->iceptInvd = 1;
		vmcb->iceptSkinit = 1;
		vmcb->iceptXsetbv = 1;
		vmcb->iceptRdpru = 1;
		vmcb->iceptRsm = 1;
		// Catch triple faults of the guest so they don't shut down the physical CPU.
		vmcb->iceptShutdown = 1;

		vmcb->iceptEferWrite = 1;
		vmcb->vIntMasking = 1;

		vmcb->nptEnable = 1;
		vmcb->nptCr3 = npt->rootTable();
		vmcb->pat = 0x0007040600070406; // State at reset

		// TODO: Don't use a single ASID to no longer flush entire TLB on vmentry
		vmcb->guestAsid = 1;
		vmcb->tlbControl = 1;

		auto iopmOutcome = physicalAllocator->tryAllocate({AllocationClass::kernel, 0},
				IOPM_BITMAP_SIZE, PhysicalKind::pinned);
		if(!iopmOutcome)
			panicLogger() << "thor: Failed to allocate the SVM I/O permission map" << frg::endlog;
		iopm_bitmap = *iopmOutcome;
		PageAccessor iopmAccessor{iopm_bitmap};
		memset(iopmAccessor.get(), 0xFF, IOPM_BITMAP_SIZE);

		auto msrpmOutcome = physicalAllocator->tryAllocate({AllocationClass::kernel, 0},
				MSRPM_BITMAP_SIZE, PhysicalKind::pinned);
		if(!msrpmOutcome)
			panicLogger() << "thor: Failed to allocate the SVM MSR permission map" << frg::endlog;
		msrpm_bitmap = *msrpmOutcome;
		PageAccessor msrpmAccessor{msrpm_bitmap};
		memset(msrpmAccessor.get(), 0xFF, MSRPM_BITMAP_SIZE);

		vmcb->iceptIo = 1;
		vmcb->iopmBasePa = iopm_bitmap;

		vmcb->iceptMsr = 1;
		vmcb->msrpmBasePa = msrpm_bitmap;

		// VMRUN requires the guest's EFER.SVME to be set, see storeRegs() and loadRegs().
		vmcb->efer = EFER_SVME;
		vmcb->cr0 = (1 << 4) | (1 << 29) | (1 << 30); // cr0 state at CPU reset
		vmcb->dr6 = 0xFFFF0FF0; // State at reset
		vmcb->dr7 = 0x400; // State at reset

		auto simd_state_size = Executor::determineSimdSize(); 

		host_fpu_state = (uint8_t *)kernelAlloc->allocate(simd_state_size);
		memset((void *)host_fpu_state, 0, simd_state_size);

		guest_fpu_state = (uint8_t *)kernelAlloc->allocate(simd_state_size);
		memset((void *)guest_fpu_state, 0, simd_state_size);
	}

	Vcpu::~Vcpu() {
		physicalAllocator->free((size_t)vmcb_region, kPageSize);
		physicalAllocator->free((size_t)host_additional_save_region, kPageSize);

		physicalAllocator->free((size_t)iopm_bitmap, IOPM_BITMAP_SIZE);
		physicalAllocator->free((size_t)msrpm_bitmap, MSRPM_BITMAP_SIZE);

		kernelAlloc->free(guest_fpu_state);
		kernelAlloc->free(host_fpu_state);
	}

	frg::expected<Error, HelVmexitReason> Vcpu::run(const HelVmexitCompletion *completion) {
		auto rax = vmcb->rax;
		auto rbx = gprState.rbx;
		auto rcx = gprState.rcx;
		auto rdx = gprState.rdx;

		bool skipCompleted;
		if(auto error = applyCompletion(completion, rax, rbx, rcx, rdx, skipCompleted); error)
			return *error;

		vmcb->rax = rax;
		gprState.rbx = rbx;
		gprState.rcx = rcx;
		gprState.rdx = rdx;

		if(skipCompleted) {
			vmcb->rip = vmcb->rip + pendingCompletion_.instructionLength;
			vmcb->irqShadow = 0;
		}

		HelVmexitReason reason{};
		while(true) {
			if(getCurrentThread()->checkCancelConditions())
				return Error::cancelled;

			lapic_.checkTimer(getClockNanos());

			// HLT ends once an interrupt can be delivered.
			if(halted_) {
				bool interruptsEnabled = vmcb->rflags & (1 << 9);
				if(interruptsEnabled && lapic_.deliverableVector() >= 0) {
					halted_ = false;
				}else{
					if(consumeKick()) {
						reason.exitReason = kHelVmexitKicked;
						return reason;
					}
					waitForGuestInterrupt(lapic_.timerDeadline(), interruptsEnabled);
					continue;
				}
			}

			vmcb->vTpr = lapic_.cr8();

			asm volatile("clgi" : : : "memory");

			if(enterGuest(getCpuData())) {
				asm volatile("stgi" : : : "memory");
				reason.exitReason = kHelVmexitKicked;
				return reason;
			}

			// An interrupted or queued event takes precedence over interrupts from the local APIC.
			// If the guest cannot accept an interrupt, V_IRQ makes it exit (VINTR) once it can.
			auto wantWindow = false;
			auto vector = lapic_.deliverableVector();
			std::optional<Event> injected;
			if(reinjectValid_) {
				wantWindow = vector >= 0;
				injected = reinject_;
				reinjectValid_ = false;
			}else if(vector >= 0) {
				if(!(vmcb->rflags & (1 << 9)) || vmcb->irqShadow) {
					wantWindow = true;
				}else{
					lapic_.acknowledge(vector);
					injected = Event{
						.type = x86_virt::kEventExternalInterrupt,
						.vector = static_cast<uint8_t>(vector),
						.hasErrorCode = false,
						.errorCode = 0,
						.instructionLength = 0,
					};
				}
			}
			if(injected) {
				uint64_t inject = injected->vector
						| (static_cast<uint64_t>(injected->type) << 8)
						| (UINT64_C(1) << 31);
				// Error codes are only delivered if guest's CR0.PE is set.
				if(injected->hasErrorCode && (vmcb->cr0 & CR0_PE))
					inject |= (UINT64_C(1) << 11) | (static_cast<uint64_t>(injected->errorCode) << 32);
				vmcb->eventInject = inject;
			}
			vmcb->vIrq = wantWindow;
			vmcb->vIgnoreTpr = wantWindow;
			vmcb->iceptVintr = wantWindow;

			auto pat = common::x86::rdmsr(common::x86::kMsrPAT);
			asm volatile("vmsave %%rax" : : "a"(host_additional_save_region) : "memory"); // Use vmsave / vmload to save additional state that would otherwise have to be wrmsr'ed

			if(getGlobalCpuFeatures()->haveXsave){
				common::x86::xsave((uint8_t *)host_fpu_state, ~0);
				common::x86::xrstor((uint8_t *)guest_fpu_state, ~0);
			} else {
				asm volatile ("fxsaveq %0" : : "m" (*host_fpu_state));
				asm volatile ("fxrstorq %0" : : "m" (*guest_fpu_state));
			}
			// XCR0 is only observable by the guest if it set CR4.OSXSAVE.
			// The FPU state is saved and restored using the (larger) host XCR0.
			auto hostXcr0 = getGlobalCpuFeatures()->xcr0Mask;
			auto switchXcr0 = getGlobalCpuFeatures()->haveXsave
					&& (vmcb->cr4 & CR4_OSXSAVE) && guestXcr0_ != hostXcr0;
			if(switchXcr0)
				common::x86::wrxcr(0, guestXcr0_);

			svmVmRun(&gprState, vmcb_region);
			leaveGuest();

			if(switchXcr0)
				common::x86::wrxcr(0, hostXcr0);

			common::x86::wrmsr(common::x86::kMsrPAT, pat);
			asm volatile("vmload %%rax" : : "a"(host_additional_save_region) : "memory");

			if(getGlobalCpuFeatures()->haveXsave){
				common::x86::xsave((uint8_t *)guest_fpu_state, ~0);
				common::x86::xrstor((uint8_t *)host_fpu_state, ~0);
			} else {
				asm volatile ("fxsaveq %0" : : "m" (*guest_fpu_state));
				asm volatile ("fxrstorq %0" : : "m" (*host_fpu_state));
			}

			asm volatile("stgi" : : : "memory");

			// Guest writes to CR8 change V_TPR without exiting.
			lapic_.setCr8(vmcb->vTpr);

			// The exit interrupted the delivery of an event.
			// Software interrupts and exceptions are not reinjected.
			auto info = vmcb->exitintinfo;
			if(info & (UINT64_C(1) << 31)) {
				uint8_t type = (info >> 8) & 7;
				uint8_t vector = info & 0xFF;
				bool software = type == x86_virt::kEventSoftwareInterrupt
						|| (type == x86_virt::kEventHardwareException && (vector == 3 || vector == 4));
				if(!software) {
					reinjectValid_ = true;
					reinject_ = {type, vector, static_cast<bool>(info & (1 << 11)),
							static_cast<uint32_t>(info >> 32), 0};
				}
			}

			auto code = vmcb->exitcode;
			switch(code) {
			default:
				infoLogger() << "svm: Unknown exitcode: " << code << frg::endlog;
				reason.exitReason = kHelVmexitUnknownPlatformSpecificExitCode;
				reason.code = code;
				return reason;

			case kSvmExitIntr:
			case kSvmExitNmi:
			case kSvmExitSmi:
				// The host handled the interrupt (or NMI, SMI) when we executed STGI.
				break;

			case kSvmExitVintr:
				// The interrupt window is handled before the next entry.
				break;

			case kSvmExitHlt:
				vmcb->rip = vmcb->nextRip;
				vmcb->irqShadow = 0;
				halted_ = true;
				break;

			case kSvmExitCpuid:
				return requestCpuidCompletion(vmcb->rax, gprState.rcx, vmcb->nextRip - vmcb->rip);

			case kSvmExitVmmcall:
				return requestHyperCallCompletion(vmcb->nextRip - vmcb->rip);

			case kSvmExitIoio: {
				uint64_t info = vmcb->exitinfo1;
				return requestIoCompletion(info >> 16, (info >> 4) & 7, info & (1 << 0),
						info & (1 << 2), vmcb->rax, vmcb->nextRip - vmcb->rip);
			}

			case kSvmExitXsetbv: {
				uint32_t index = gprState.rcx;
				uint64_t value = (gprState.rdx << 32) | static_cast<uint32_t>(vmcb->rax);
				if(!(vmcb->cr4 & CR4_OSXSAVE)) {
					queueException(x86_virt::UD_VECTOR, false, 0);
					break;
				}
				if(vmcb->cpl || index != 0 || !x86_virt::isValidXcr0(value)) {
					queueException(x86_virt::GP_VECTOR, true, 0);
					break;
				}
				guestXcr0_ = value;
				vmcb->rip = vmcb->nextRip;
				vmcb->irqShadow = 0;
				break;
			}

			case kSvmExitMsr: {
				uint32_t index = gprState.rcx;
				if(!isVirtualLapicMsr(index)) {
					return requestMsrCompletion(vmcb->exitinfo1 & 1, vmcb->rax, gprState.rcx,
							gprState.rdx, vmcb->nextRip - vmcb->rip);
				}

				auto rax = vmcb->rax;
				auto rdx = gprState.rdx;
				VirtualLapic::WriteEffects effects;
				if(emulateLapicMsr(index, vmcb->exitinfo1 & 1, getClockNanos(), effects, rax, rdx)) {
					vmcb->rax = rax;
					gprState.rdx = rdx;
					vmcb->rip = vmcb->nextRip;
					vmcb->irqShadow = 0;
				}

				// The VMM (which models the I/O APIC) needs to know about EOIs of
				// level-triggered interrupts.
				if(effects.levelEoi >= 0) {
					reason.exitReason = kHelVmexitEoi;
					reason.code = effects.levelEoi;
					return reason;
				}
				break;
			}

			case kSvmExitExceptionDb:
				// The CPU already updated DR6 in the VMCB.
				queueException(DB_VECTOR, false, 0);
				break;

			case kSvmExitExceptionAc:
				queueException(AC_VECTOR, true, vmcb->exitinfo1);
				break;

			case kSvmExitShutdown:
				// The VMCB is undefined after a shutdown, the VMM needs to reset the vCPU.
				reason.exitReason = kHelVmexitShutdown;
				return reason;

			case kSvmExitNPTFault: {
				size_t address = vmcb->exitinfo2;
				size_t exitFlags = vmcb->exitinfo1;
				uint32_t flags = AddressSpace::kFaultSynchronous;
				if(exitFlags & (1 << 1))
					flags |= AddressSpace::kFaultWrite;
				if(exitFlags & (1 << 4))
					flags |= AddressSpace::kFaultExecute;

				auto faultOutcome = Thread::asyncBlockCurrent(
					space->handleFault(address, flags),
					getCurrentThread()->pagingWorkQueue().get()
				);
				if(!faultOutcome) {
					reason.exitReason = kHelVmexitTranslationFault;
					reason.address = address;
					if(exitFlags & (1 << 1))
						reason.flags = kHelVmFaultWrite;
					else if(exitFlags & (1 << 4))
						reason.flags = kHelVmFaultExecute;
					else
						reason.flags = kHelVmFaultRead;
					return reason;
				}
				
				break;
			}

			case ~UINT64_C(0): // Invalid VMCB
				reason.exitReason = kHelVmexitError;
				return reason;
			}
		}
	}

	void Vcpu::storeRegs(const HelX86VirtualizationRegs *regs) {
		vmcb->rax = regs->rax;
		gprState.rbx = regs->rbx;
		gprState.rcx = regs->rcx;
		gprState.rdx = regs->rdx;
		gprState.rsi = regs->rsi;
		gprState.rdi = regs->rdi;
		gprState.rbp = regs->rbp;

		gprState.r8 = regs->r8;
		gprState.r9 = regs->r9;
		gprState.r10 = regs->r10;
		gprState.r11 = regs->r11;
		gprState.r12 = regs->r12;
		gprState.r13 = regs->r13;
		gprState.r14 = regs->r14;
		gprState.r15 = regs->r15;

		vmcb->rip = regs->rip;
		vmcb->rsp = regs->rsp;
		vmcb->rflags = regs->rflags;

		vmcb->cr0 = regs->cr0;
		vmcb->cr2 = regs->cr2;
		vmcb->cr3 = regs->cr3;
		vmcb->cr4 = regs->cr4;

		// VMRUN fails if EFER.SVME is clear.
		vmcb->efer = regs->efer | EFER_SVME;

		lapic_.setCr8(regs->cr8);
		// regs->apic_base is ignored: the APIC base is fixed.

		vmcb->gdt.base = regs->gdt.base;
		vmcb->gdt.limit = regs->gdt.limit;

		vmcb->idt.base = regs->idt.base;
		vmcb->idt.limit = regs->idt.limit;

		// The VMCB has no unusable bit; unusable segments are simply marked not present.
		#define SET_SEGMENT(segment) \
			vmcb->segment.base = regs->segment.base; \
			vmcb->segment.limit = regs->segment.limit; \
			vmcb->segment.selector = regs->segment.selector; \
			vmcb->segment.attrib = regs->segment.type | (regs->segment.s << 4) | \
								   (regs->segment.dpl << 5) | \
								   ((regs->segment.present && !regs->segment.unusable) << 7) | \
								   (regs->segment.avl << 8) | (regs->segment.l << 9) | \
								   (regs->segment.db << 10) | (regs->segment.g << 11)

		SET_SEGMENT(cs);
		SET_SEGMENT(ds);
		SET_SEGMENT(ss);
		SET_SEGMENT(es);
		SET_SEGMENT(fs);
		SET_SEGMENT(gs);

		SET_SEGMENT(ldt);
		SET_SEGMENT(tr);

		// The CPU tracks the CPL in the VMCB.
		vmcb->cpl = regs->ss.dpl;
	}

	void Vcpu::loadRegs(HelX86VirtualizationRegs *regs) {
		regs->rax = vmcb->rax;
		regs->rbx = gprState.rbx;
		regs->rcx = gprState.rcx;
		regs->rdx = gprState.rdx;
		regs->rsi = gprState.rsi;
		regs->rdi = gprState.rdi;
		regs->rbp = gprState.rbp;

		regs->r8 = gprState.r8;
		regs->r9 = gprState.r9;
		regs->r10 = gprState.r10;
		regs->r11 = gprState.r11;
		regs->r12 = gprState.r12;
		regs->r13 = gprState.r13;
		regs->r14 = gprState.r14;
		regs->r15 = gprState.r15;

		regs->rip = vmcb->rip;
		regs->rsp = vmcb->rsp;
		regs->rflags = vmcb->rflags;

		regs->cr0 = vmcb->cr0;
		regs->cr2 = vmcb->cr2;
		regs->cr3 = vmcb->cr3;
		regs->cr4 = vmcb->cr4;

		// Hide EFER.SVME from the guest since it was not set by it.
		regs->efer = vmcb->efer & ~EFER_SVME;

		regs->cr8 = lapic_.cr8();
		regs->apic_base = VirtualLapic::apicBase();

		regs->gdt.base = vmcb->gdt.base;
		regs->gdt.limit = vmcb->gdt.limit;

		regs->idt.base = vmcb->idt.base;
		regs->idt.limit = vmcb->idt.limit;

		#define GET_SEGMENT(segment) \
			regs->segment.base = vmcb->segment.base; \
			regs->segment.limit = vmcb->segment.limit; \
			regs->segment.selector = vmcb->segment.selector; \
			regs->segment.type = vmcb->segment.attrib & 0xF; \
			regs->segment.s = (vmcb->segment.attrib >> 4) & 1; \
			regs->segment.dpl = (vmcb->segment.attrib >> 5) & 0b11; \
			regs->segment.present = (vmcb->segment.attrib >> 7) & 1; \
			regs->segment.avl = (vmcb->segment.attrib >> 8) & 1; \
			regs->segment.l = (vmcb->segment.attrib >> 9) & 1; \
			regs->segment.db = (vmcb->segment.attrib >> 10) & 1; \
			regs->segment.g = (vmcb->segment.attrib >> 11) & 1; \
			regs->segment.unusable = !regs->segment.present

		GET_SEGMENT(cs);
		GET_SEGMENT(ds);
		GET_SEGMENT(ss);
		GET_SEGMENT(es);
		GET_SEGMENT(fs);
		GET_SEGMENT(gs);

		GET_SEGMENT(ldt);
		GET_SEGMENT(tr);

		// SS.DPL in the VMCB can be stale.
		regs->ss.dpl = vmcb->cpl;
	}

	bool Vcpu::storeMsrs(const HelX86VirtualizationMsrs *msrs) {
		if(!x86_virt::isValidXcr0(msrs->xcr0))
			return false;
		if(!x86_virt::isCanonical(msrs->kernelGsBase) || !x86_virt::isCanonical(msrs->lstar)
				|| !x86_virt::isCanonical(msrs->cstar) || (msrs->sfmask >> 32)
				|| !x86_virt::isCanonical(msrs->sysenterEsp)
				|| !x86_virt::isCanonical(msrs->sysenterEip))
			return false;

		vmcb->kernel_gs_base = msrs->kernelGsBase;
		vmcb->star = msrs->star;
		vmcb->lstar = msrs->lstar;
		vmcb->cstar = msrs->cstar;
		vmcb->sfmask = msrs->sfmask;
		vmcb->sysenter_cs = msrs->sysenterCs;
		vmcb->sysenter_esp = msrs->sysenterEsp;
		vmcb->sysenter_eip = msrs->sysenterEip;
		guestXcr0_ = msrs->xcr0;
		return true;
	}

	void Vcpu::loadMsrs(HelX86VirtualizationMsrs *msrs) {
		msrs->kernelGsBase = vmcb->kernel_gs_base;
		msrs->star = vmcb->star;
		msrs->lstar = vmcb->lstar;
		msrs->cstar = vmcb->cstar;
		msrs->sfmask = vmcb->sfmask;
		msrs->sysenterCs = vmcb->sysenter_cs;
		msrs->sysenterEsp = vmcb->sysenter_esp;
		msrs->sysenterEip = vmcb->sysenter_eip;
		msrs->xcr0 = guestXcr0_;
	}
}
