#include <thor-internal/arch/svm.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>
#include <x86/machine.hpp>

extern "C" void svmVmRun(thor::svm::GprState *gprs, PhysicalAddr vmcb);

namespace {

constexpr uint64_t EFER_SVME = 1 << 12;
constexpr uint64_t CR0_PE = 1 << 0;

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
		if(completion)
			return Error::illegalState;
		HelVmexitReason reason{};
		while(true) {
			if(getCurrentThread()->checkCancelConditions())
				return Error::cancelled;

			if(reinjectValid_) {
				uint64_t inject = reinject_.vector
						| (static_cast<uint64_t>(reinject_.type) << 8)
						| (UINT64_C(1) << 31);
				// Error codes are only delivered if guest's CR0.PE is set.
				if(reinject_.hasErrorCode && (vmcb->cr0 & CR0_PE))
					inject |= (UINT64_C(1) << 11) | (static_cast<uint64_t>(reinject_.errorCode) << 32);
				vmcb->eventInject = inject;
				reinjectValid_ = false;
			}

			asm volatile("clgi" : : : "memory");

			if(enterGuest(getCpuData())) {
				asm volatile("stgi" : : : "memory");
				reason.exitReason = kHelVmexitKicked;
				return reason;
			}

			auto pat = common::x86::rdmsr(common::x86::kMsrPAT);
			asm volatile("vmsave %%rax" : : "a"(host_additional_save_region) : "memory"); // Use vmsave / vmload to save additional state that would otherwise have to be wrmsr'ed

			if(getGlobalCpuFeatures()->haveXsave){
				common::x86::xsave((uint8_t *)host_fpu_state, ~0);
				common::x86::xrstor((uint8_t *)guest_fpu_state, ~0);
			} else {
				asm volatile ("fxsaveq %0" : : "m" (*host_fpu_state));
				asm volatile ("fxrstorq %0" : : "m" (*guest_fpu_state));
			}

			svmVmRun(&gprState, vmcb_region);
			leaveGuest();

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

			case kSvmExitHlt:
				reason.exitReason = kHelVmexitHlt;
				return reason;

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
					reason.flags = exitFlags;
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
		gprState.cr2 = regs->cr2;
		vmcb->cr3 = regs->cr3;
		vmcb->cr4 = regs->cr4;

		// VMRUN fails if EFER.SVME is clear.
		vmcb->efer = regs->efer | EFER_SVME;

		vmcb->gdt.base = regs->gdt.base;
		vmcb->gdt.limit = regs->gdt.limit;

		vmcb->idt.base = regs->idt.base;
		vmcb->idt.limit = regs->idt.limit;

		#define SET_SEGMENT(segment) \
			vmcb->segment.base = regs->segment.base; \
			vmcb->segment.limit = regs->segment.limit; \
			vmcb->segment.selector = regs->segment.selector; \
			vmcb->segment.attrib = regs->segment.type | (regs->segment.s << 4) | \
								   (regs->segment.dpl << 5) | (regs->segment.present << 7) | \
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
		regs->cr2 = gprState.cr2;
		regs->cr3 = vmcb->cr3;
		regs->cr4 = vmcb->cr4;

		// Hide EFER.SVME from the guest since it was not set by it.
		regs->efer = vmcb->efer & ~EFER_SVME;

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
			regs->segment.g = (vmcb->segment.attrib >> 11) & 1

		GET_SEGMENT(cs);
		GET_SEGMENT(ds);
		GET_SEGMENT(ss);
		GET_SEGMENT(es);
		GET_SEGMENT(fs);
		GET_SEGMENT(gs);

		GET_SEGMENT(ldt);
		GET_SEGMENT(tr);
	}

	bool Vcpu::assertInterrupt(uint64_t number, bool level) {
		// TODO: Implement using virtualized LAPIC/interrupts.
		(void)number;
		(void)level;
		return false;
	}
}
