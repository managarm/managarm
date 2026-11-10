#include <optional>

#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>
#include <x86/machine.hpp>

namespace {
	inline int vmptrld(PhysicalAddr vmcs) {
		uint8_t ret;
		asm volatile (
			"vmptrld %[pa];"
			"setna %[ret];"
			: [ret]"=rm"(ret)
			: [pa]"m"(vmcs)
			: "cc", "memory");
		return ret;
	}

	inline int vmclear(PhysicalAddr vmcs) {
		uint8_t ret;
		asm volatile (
			"vmclear %[pa];"
			"setna %[ret];"
			: [ret]"=rm"(ret)
			: [pa]"m"(vmcs)
			: "cc", "memory");
		return ret;
	}

	inline int vmwrite(uint64_t encoding, uint64_t value) {
		uint8_t ret;
		asm volatile (
			"vmwrite %1, %2;"
			"setna %[ret]"
			: [ret]"=rm"(ret)
			: "rm"(value), "r"(encoding)
			: "cc", "memory");

		return ret;
	}

	inline uint64_t vmread(uint64_t encoding) {
		uint64_t tmp;
		uint8_t ret;
		asm volatile(
			"vmread %[encoding], %[value];"
			"setna %[ret];"
			: [value]"=rm"(tmp), [ret]"=rm"(ret)
			: [encoding]"r"(encoding)
			: "cc", "memory");

		return tmp;
	}

	inline uint32_t adjustControls(uint64_t msr, uint32_t desired, uint32_t required, const char *name) {
		auto caps = common::x86::rdmsr(msr);
		// The low half of the IA32_VMX_*_CTLS MSRs holds the controls that must be 1
		// and the high half holds the controls that may be 1.
		uint32_t controls = (desired | static_cast<uint32_t>(caps)) & static_cast<uint32_t>(caps >> 32);
		if((controls & required) != required) {
			thor::infoLogger() << "vmx: CPU lacks required " << name << " controls 0x"
					<< frg::hex_fmt{required & ~controls} << frg::endlog;
		}
		return controls;
	}

	inline bool haveTrueControls() {
		return common::x86::rdmsr(thor::vmx::IA32_VMX_BASIC_MSR) & (UINT64_C(1) << 55);
	}

	// Writes the host selectors, which are the same on all CPUs.
	void writeHostSelectors() {
		uint16_t es, cs, ss, ds, fs, gs, tr;
		asm volatile ("str %[tr]" : [tr]"=rm"(tr));
		asm volatile ("mov %%es,%0" : "=r"(es));
		asm volatile ("mov %%cs,%0" : "=r"(cs));
		asm volatile ("mov %%ss,%0" : "=r"(ss));
		asm volatile ("mov %%ds,%0" : "=r"(ds));
		asm volatile ("mov %%fs,%0" : "=r"(fs));
		asm volatile ("mov %%gs,%0" : "=r"(gs));

		vmwrite(thor::vmx::HOST_ES_SELECTOR, es);
		vmwrite(thor::vmx::HOST_CS_SELECTOR, cs);
		vmwrite(thor::vmx::HOST_SS_SELECTOR, ss);
		vmwrite(thor::vmx::HOST_DS_SELECTOR, ds);
		vmwrite(thor::vmx::HOST_FS_SELECTOR, fs);
		vmwrite(thor::vmx::HOST_GS_SELECTOR, gs);
		vmwrite(thor::vmx::HOST_TR_SELECTOR, tr);
	}

	// Writes the host state that differs between CPUs.
	void writeHostCpuState() {
		vmwrite(thor::vmx::HOST_GS_BASE, common::x86::rdmsr(thor::vmx::MSR_GS_BASE));

		common::x86::Gdtr gdtr;
		asm volatile("sgdt %[gdt]": [gdt]"=m"(gdtr));
		common::x86::Idtr idtr;
		asm volatile("sidt %[idt]": [idt]"=m"(idtr));

		uint32_t* gdt = gdtr.pointer;
		uint32_t entry1 = (gdt[thor::kGdtIndexTask * 2] >> 16) & 0xFFFF;
		uint32_t entry2 = (gdt[thor::kGdtIndexTask * 2 + 1]) & 0xFF;
		uint32_t entry3 = (gdt[thor::kGdtIndexTask * 2 + 1] >> 24) & 0xFF;
		uint32_t entry4 = (gdt[thor::kGdtIndexTask * 2 + 2]);
		uint64_t trAddr = ((uint64_t)entry4 << 32) |
			(entry1 | ((entry2) << 16) | ((entry3) << 24));

		vmwrite(thor::vmx::HOST_TR_BASE, trAddr);
		vmwrite(thor::vmx::HOST_GDTR_BASE, (size_t)gdtr.pointer);
		vmwrite(thor::vmx::HOST_IDTR_BASE, (size_t)idtr.pointer);
	}
}

extern "C" bool vmxVmRun(thor::vmx::Vmcs* vm, void* state, bool launched);

namespace thor::vmx {
	// The VMCS that is current on each CPU (VMPTRLD), or null.
	extern PerCpu<Vmcs *> currentVmcs;
	THOR_DEFINE_PERCPU(currentVmcs);
}

extern "C" uintptr_t vmxDoVmExit[]; // Actually a function that cannot be called in the conventional sense, just need the address

extern "C" void vmxUpdateHostRsp(thor::vmx::Vmcs* vm, uintptr_t rsp) {
	if(vm->saved_host_rsp != rsp) {
		vmwrite(thor::vmx::HOST_RSP, rsp);
		vm->saved_host_rsp = rsp;
	}
}

namespace thor::vmx {
	bool vmxon() {
		infoLogger() << "vmx: enabling vmx" << frg::endlog;

		auto vmxonRegion = physicalAllocator->allocateInfallible(PhysicalKind::pinned);

		PageAccessor vmxonAccessor{vmxonRegion};
		memset(vmxonAccessor.get(), 0, kPageSize);
		size_t control = common::x86::rdmsr(0x3a);
		if((control & (0x1 | 0x4)) != (0x1 | 0x04)) {
			//Enabled outside of SMX and lock bit.
			common::x86::wrmsr(0x3a, control | 0x1 | 0x4);
		}

		uint64_t cr0;
		asm volatile ("mov %%cr0, %0" : "=r" (cr0));
		cr0 &= common::x86::rdmsr(0x487);
		cr0 |= common::x86::rdmsr(0x486);
		asm volatile ("mov %0, %%cr0" : : "r" (cr0));

		uint64_t cr4;
		asm volatile ("mov %%cr4, %0" : "=r" (cr4));
		cr4 |= 1 << 13;
		cr4 &= common::x86::rdmsr(0x489);
		cr4 |= common::x86::rdmsr(0x488);
		asm volatile ("mov %0, %%cr4" : : "r" (cr4));

		eptAccessedDirty = common::x86::rdmsr(IA32_VMX_EPT_VPID_CAP_MSR) & (1 << 21);

		//Set vmx revision
		uint32_t vmxRevision = common::x86::rdmsr(0x480);
		*(uint32_t*)vmxonAccessor.get() = static_cast<uint32_t>(vmxRevision);
		uint16_t successful = 0;
		asm volatile(
			"vmxon %1;"
			"jnc success;"
			"movq $0, %%rdx;"
			"success:"
			"movq $1, %%rdx;"
			:"=d"(successful)
			:"m"(vmxonRegion)
			:"memory", "cc"
		);

		if(successful) {
			infoLogger() << "thor: CPU entered vmxon operation" << frg::endlog;
		} else {
			urgentLogger() << "thor: vmxon failed; this will be a hard error in the future"
					<< frg::endlog;
		}

		return successful;
	}

	Vmcs::Vmcs(CtorToken, smarter::shared_ptr<EptSpace> ept)
	: space(std::move(ept)) {
		infoLogger() << "vmx: Creating VMCS" << frg::endlog;
		region = (void*)physicalAllocator->allocateInfallible(PhysicalKind::pinned);
		PageAccessor regionAccessor{(PhysicalAddr)region};
		memset(regionAccessor.get(), 0, kPageSize);
		uint32_t vmxRevision = common::x86::rdmsr(0x480);
		*(uint32_t*)regionAccessor.get() = static_cast<uint32_t>(vmxRevision);

		// A VMCS is only active (and thus may be cached by the CPU) while we cannot migrate
		// to another CPU. Afterwards, we VMCLEAR it (see acquire_() and release_()).
		StatelessIrqLock irqLock;
		vmclear((PhysicalAddr)region);
		acquire_();

		//Set up basic controls.
		auto trueControls = haveTrueControls();
		auto pinbasedControls = 1; // External interrupt exiting.
		vmwrite(PIN_BASED_VM_EXEC_CONTROLS, adjustControls(
				trueControls ? IA32_VMX_TRUE_PINBASED_CTLS_MSR : MSR_IA32_VMX_PINBASED_CTLS,
				pinbasedControls, pinbasedControls, "pin-based"));
		// Without a TPR shadow, CR8 accesses would otherwise reach the host's TPR.
		uint32_t procControls = VMEXIT_ON_HLT
				| VMEXIT_ON_PIO
				| SECONDARY_CONTROLS_ON
				| CR8_LOAD_EXITING
				| CR8_STORE_EXITING;
		vmwrite(PROC_BASED_VM_EXEC_CONTROLS, adjustControls(
				trueControls ? IA32_VMX_TRUE_PROCBASED_CTLS_MSR : MSR_IA32_VMX_PROCBASED_CTLS,
				procControls, procControls, "processor-based"));
		uint32_t secondaryControls = EPT_ENABLE | UNRESTRICTED_GUEST;
		vmwrite(PROC_BASED_VM_EXEC_CONTROLS2, adjustControls(IA32_VMX_SEC_PROCBASED_CTLS_MSR,
				secondaryControls, secondaryControls, "secondary processor-based"));
		vmwrite(EXCEPTION_BITMAP, 0);

		uint32_t exitControls = VMEXIT_CONTROLS_LONG_MODE | VMEXIT_CONTROLS_LOAD_IA32_EFER;
		// Saving the guest's EFER keeps EFER.LMA up to date (the CPU sets it when entering long mode).
		vmwrite(VM_EXIT_CONTROLS, adjustControls(
				trueControls ? IA32_VMX_TRUE_EXIT_CTLS_MSR : MSR_IA32_VMX_EXIT_CTLS,
				exitControls | VMEXIT_CONTROLS_SAVE_IA32_EFER, exitControls, "VM-exit"));

		uint32_t entryControls = 1 << 15; // Load IA32_EFER.
		entryControls_ = adjustControls(
				trueControls ? IA32_VMX_TRUE_ENTRY_CTLS_MSR : MSR_IA32_VMX_ENTRY_CTLS,
				entryControls, entryControls, "VM-entry") & ~VM_ENTRY_IA32E_MODE;
		vmwrite(VM_ENTRY_CONTROLS, entryControls_);

		uint64_t cr0, cr4;
		asm volatile ("mov %%cr0,%0" : "=r"(cr0));
		asm volatile ("mov %%cr4,%0" : "=r"(cr4));

		//Set up host state on vmexit.
		uint32_t cr0Fixed = (uint32_t)common::x86::rdmsr(IA32_VMX_CR0_FIXED0_MSR);
		vmwrite(HOST_CR0, cr0Fixed | cr0);
		uint32_t cr4Fixed = (uint32_t)common::x86::rdmsr(IA32_VMX_CR4_FIXED0_MSR);
		vmwrite(HOST_CR4, cr4Fixed | cr4);

		vmwrite(HOST_EFER_FULL, common::x86::rdmsr(0xc0000080));
		vmwrite(HOST_RIP, (size_t)vmxDoVmExit);

		//Set up guest state on vm entry.
		vmwrite(GUEST_ES_SELECTOR, 0x0);
		vmwrite(GUEST_CS_SELECTOR, 0);
		vmwrite(GUEST_DS_SELECTOR, 0x0);
		vmwrite(GUEST_FS_SELECTOR, 0x0);
		vmwrite(GUEST_GS_SELECTOR, 0x0);
		vmwrite(GUEST_SS_SELECTOR, 0x0);
		vmwrite(GUEST_TR_SELECTOR, 0x0);
		vmwrite(GUEST_LDTR_SELECTOR, 0x0);
		vmwrite(GUEST_CS_BASE, 0x0);
		vmwrite(GUEST_DS_BASE, 0x0);
		vmwrite(GUEST_ES_BASE, 0x0);
		vmwrite(GUEST_FS_BASE, 0x0);
		vmwrite(GUEST_GS_BASE, 0x0);
		vmwrite(GUEST_SS_BASE, 0x0);
		vmwrite(GUEST_LDTR_BASE, 0x0);
		vmwrite(GUEST_IDTR_BASE, 0x0);
		vmwrite(GUEST_GDTR_BASE, 0x0);
		vmwrite(GUEST_TR_BASE, 0x0);
		vmwrite(GUEST_CS_LIMIT, 0xffff);
		vmwrite(GUEST_DS_LIMIT, 0xffff);
		vmwrite(GUEST_ES_LIMIT, 0xffff);
		vmwrite(GUEST_FS_LIMIT, 0xffff);
		vmwrite(GUEST_GS_LIMIT, 0xffff);
		vmwrite(GUEST_SS_LIMIT, 0xffff);
		vmwrite(GUEST_LDTR_LIMIT, 0xffff);
		vmwrite(GUEST_TR_LIMIT, 0xffff);
		vmwrite(GUEST_GDTR_LIMIT, 0xffff);
		vmwrite(GUEST_IDTR_LIMIT, 0xffff);

		vmwrite(GUEST_CS_ACCESS_RIGHT, CODE_ACCESS_RIGHT);
		vmwrite(GUEST_DS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_ES_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_FS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_GS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_SS_ACCESS_RIGHT, DATA_ACCESS_RIGHT);
		vmwrite(GUEST_LDTR_ACCESS_RIGHT, LDTR_ACCESS_RIGHT);
		vmwrite(GUEST_TR_ACCESS_RIGHT, TR_ACCESS_RIGHT);
		vmwrite(GUEST_INTERRUPTIBILITY_STATE, 0x0);
		vmwrite(GUEST_ACTIVITY_STATE, 0x0);
		vmwrite(GUEST_DR7, 0x0);
		vmwrite(GUEST_RSP, 0x0);
		vmwrite(GUEST_RIP, 0x1000);
		vmwrite(GUEST_RFLAG, RFLAG_RESERVED);
		vmwrite(VMCS_LINK_POINTER, -1ll);
		vmwrite(VMCS_FIELD_GUEST_EFER_FULL, 0x0);

		vmwrite(GUEST_INTR_STATUS, 0);
		vmwrite(GUEST_PML_INDEX, 0);
		uint64_t cr0FixedGuest = common::x86::rdmsr(IA32_VMX_CR0_FIXED0_MSR);
		uint32_t cr0FixedLo = (uint32_t)cr0FixedGuest;
		uint32_t cr0FixedHi = (uint32_t)(cr0FixedGuest >> 32);
		cr0FixedLo &= ~(1 << 0); // disable PE
		cr0FixedLo &= ~(1 << 31); // disable PG
		vmwrite(GUEST_CR0, cr0FixedLo | ((uint64_t)cr0FixedHi) << 32);
		vmwrite(GUEST_CR3, 0x0);
		uint64_t cr4FixedGuest = common::x86::rdmsr(IA32_VMX_CR4_FIXED0_MSR);
		uint32_t cr4FixedLo = (uint32_t)cr4FixedGuest;
		uint32_t cr4FixedHi = (uint32_t)(cr4FixedGuest >> 32);
		vmwrite(GUEST_CR4, cr4FixedLo | ((uint64_t)cr4FixedHi) << 32);

		auto eptCaps = common::x86::rdmsr(IA32_VMX_EPT_VPID_CAP_MSR);
		if(!(eptCaps & (1 << 6)) || !(eptCaps & (1 << 14)))
			infoLogger() << "vmx: CPU lacks 4-level EPT or write-back EPT structures" << frg::endlog;
		if(!(eptCaps & (1 << 25)))
			infoLogger() << "vmx: CPU lacks single-context INVEPT" << frg::endlog;
		uint64_t eptp = eptPointer(space->rootTable());
		vmwrite(CTLS_EPTP, eptp);

		release_();
		irqLock.unlock();
		state = {};


		if(getGlobalCpuFeatures()->haveXsave){
			hostFstate = (uint8_t*)kernelAlloc->allocate(getGlobalCpuFeatures()->xsaveRegionSize);
			memset((void*)hostFstate, 0, getGlobalCpuFeatures()->xsaveRegionSize);

			guestFstate = (uint8_t*)kernelAlloc->allocate(getGlobalCpuFeatures()->xsaveRegionSize);
			memset((void*)guestFstate, 0, getGlobalCpuFeatures()->xsaveRegionSize);
		} else {
			hostFstate = (uint8_t*)kernelAlloc->allocate(512);
			memset((void*)hostFstate, 0, 512);

			guestFstate = (uint8_t*)kernelAlloc->allocate(512);
			memset((void*)guestFstate, 0, 512);
		}
	}

	bool Vmcs::assertInterrupt(uint64_t number, bool level) {
		// TODO: Implement using virtualized APIC.
		(void)number;
		(void)level;
		return false;
	}

	frg::expected<Error, HelVmexitReason> Vmcs::run(const HelVmexitCompletion *completion) {
		// No exit needs a completion yet.
		if(completion)
			return Error::illegalState;

		HelVmexitReason exitInfo{};
		uint64_t hostFsBase = common::x86::rdmsr(MSR_FS_BASE);

		// While the VMCS is resident, it stays active on this CPU between entries.
		std::optional<MigrationGuard> migrationGuard;
		auto leaveResidency = [&] {
			if(!resident_)
				return;
			{
				StatelessIrqLock irqLock;
				resident_ = false;
				release_();
			}
			migrationGuard.reset();
		};
		frg::scope_exit residencyScope{leaveResidency};

		while(1) {
			if(getCurrentThread()->checkCancelConditions())
				return Error::cancelled;
			if(!resident_) {
				migrationGuard.emplace();
				resident_ = true;
			}

			bool entryFailed;
			uint64_t error, reason;

			{
				StatelessIrqLock irqLock;
				if(enterGuest(getCpuData())) {
					exitInfo.exitReason = kHelVmexitKicked;
					return exitInfo;
				}
				acquire_();
				// VMX instructions are expensive under nested virtualization; avoid needless ones.
				// The host state only differs between CPUs and threads.
				auto cpu = getCpuData();
				if(!hostSelectorsWritten_) {
					writeHostSelectors();
					hostSelectorsWritten_ = true;
				}
				if(hostStateCpu_ != cpu) {
					writeHostCpuState();
					hostStateCpu_ = cpu;
				}
				if(!hostFsBaseWritten_ || hostFsBase_ != hostFsBase) {
					vmwrite(HOST_FS_BASE, hostFsBase);
					hostFsBase_ = hostFsBase;
					hostFsBaseWritten_ = true;
				}
				// CR3 differs between threads.
				// Even for the same thread, it changes when the PCID of our address space is rebound.
				uint64_t cr3;
				asm volatile ("mov %%cr3, %0" : "=r"(cr3));
				if(cr3 != hostCr3_) {
					vmwrite(HOST_CR3, cr3);
					hostCr3_ = cr3;
				}
				if(!space->enterGuest(cpu)) {
					leaveGuest();
					return Error::illegalState;
				}
				if(getGlobalCpuFeatures()->haveXsave){
					common::x86::xsave((uint8_t*)hostFstate, ~0);
					common::x86::xrstor((uint8_t*)guestFstate, ~0);
				} else {
					asm volatile ("fxsaveq %0" : : "m" (*hostFstate));
					asm volatile ("fxrstorq %0" : : "m" (*guestFstate));
				}
				entryFailed = vmxVmRun(this, &state, launched_);
				if(!entryFailed)
					launched_ = true;
				if(getGlobalCpuFeatures()->haveXsave){
					common::x86::xsave((uint8_t*)guestFstate, ~0);
					common::x86::xrstor((uint8_t*)hostFstate, ~0);
				} else {
					asm volatile ("fxsaveq %0" : : "m" (*guestFstate));
					asm volatile ("fxrstorq %0" : : "m" (*hostFstate));
				}

				//Vm exits don't restore the gdt limit
				common::x86::Gdtr gdtr;
				asm volatile("sgdt %[gdt]": [gdt]"=m"(gdtr));
				gdtr.limit = HOST_GDT_LIMIT;
				asm volatile("lgdt %[gdt]": [gdt]"=m"(gdtr));

				error = entryFailed ? vmread(VM_INSTRUCTION_ERROR) : 0;
				reason = entryFailed ? ~UINT64_C(0) : vmread(VM_EXIT_REASON);
				space->leaveGuest();
				leaveGuest();
			}

			if(entryFailed) {
				infoLogger() << "vmx: VM entry failed with error " << error << frg::endlog;
				exitInfo.exitReason = kHelVmexitError;
				exitInfo.code = error;
				return exitInfo;
			}

			if(reason == VMEXIT_HLT) {
				infoLogger() << "vmx: hlt" << frg::endlog;
				exitInfo.exitReason = kHelVmexitHlt;
				return exitInfo;
			} else if(reason == VMEXIT_EPT_VIOLATION) {
				size_t address = vmread(EPT_VIOLATION_ADDRESS);
				size_t exitFlags = vmread(EPT_VIOLATION_FLAGS);
				uint32_t flags = AddressSpace::kFaultSynchronous;
				if(exitFlags & 1)
					flags |= AddressSpace::kFaultWrite;
				if(exitFlags & (1 << 2))
					flags |= AddressSpace::kFaultExecute;

				leaveResidency();
				auto faultOutcome = Thread::asyncBlockCurrent(
					space->handleFault(address, flags),
					getCurrentThread()->pagingWorkQueue().get()
				);
				if(!faultOutcome) {
					exitInfo.exitReason = kHelVmexitTranslationFault;
					exitInfo.address = address;
					exitInfo.flags = exitFlags;
					return exitInfo;
				}
			} else if(reason == VMEXIT_EXTERNAL_INTERRUPT) {
				infoLogger() << "vmx: external-interrupt exit" << frg::endlog;
			} else {
				infoLogger() << "vmx: Unknown VMExit code: " << reason << frg::endlog;
				exitInfo.exitReason = kHelVmexitUnknownPlatformSpecificExitCode;
				exitInfo.code = reason;
				return exitInfo;
			}
		}
	}

	void Vmcs::storeRegs(const HelX86VirtualizationRegs *regs) {
		// See the constructor.
		StatelessIrqLock irqLock;
		acquire_();

		vmwrite(GUEST_RSP, regs->rsp);
		vmwrite(GUEST_RIP, regs->rip);

		state.rax = regs->rax;
		state.rbx = regs->rbx;
		state.rcx = regs->rcx;
		state.rdx = regs->rdx;
		state.rsi = regs->rsi;
		state.rdi = regs->rdi;
		state.rbp = regs->rbp;
		state.r8 = regs->r8;
		state.r9 = regs->r9;
		state.r10 = regs->r10;
		state.r11 = regs->r11;
		state.r12 = regs->r12;
		state.r13 = regs->r13;
		state.r14 = regs->r14;
		state.r15 = regs->r15;
		state.cr2 = regs->cr2;

		#define SET_SEGMENT(segment, segment_capital) \
            vmwrite(GUEST_##segment_capital##_BASE, regs->segment.base); \
            vmwrite(GUEST_##segment_capital##_LIMIT, regs->segment.limit); \
            vmwrite(GUEST_##segment_capital##_SELECTOR, regs->segment.selector); \
            { \
                uint32_t attrib = regs->segment.type | (regs->segment.s << 4) | \
                                  (regs->segment.dpl << 5) | (regs->segment.present << 7) | \
                                  (regs->segment.avl << 12) | (regs->segment.l << 13) | \
                                  (regs->segment.db << 14) | (regs->segment.g << 15) | \
                                  (regs->segment.unusable << 16); \
                vmwrite(GUEST_##segment_capital##_ACCESS_RIGHT, attrib); \
            }

		SET_SEGMENT(cs, CS);
		SET_SEGMENT(ds, DS);
		SET_SEGMENT(ss, SS);
		SET_SEGMENT(es, ES);
		SET_SEGMENT(fs, FS);
		SET_SEGMENT(gs, GS);

		SET_SEGMENT(ldt, LDTR);
		SET_SEGMENT(tr, TR);

		vmwrite(GUEST_GDTR_BASE, regs->gdt.base);
		vmwrite(GUEST_GDTR_LIMIT, regs->gdt.base);

		vmwrite(GUEST_IDTR_BASE, regs->idt.base);
		vmwrite(GUEST_IDTR_LIMIT, regs->idt.base);


		vmwrite(GUEST_CR0, regs->cr0);
		vmwrite(GUEST_CR3, regs->cr3);
		vmwrite(GUEST_CR4, regs->cr4);

		vmwrite(VMCS_FIELD_GUEST_EFER_FULL, regs->efer);
		release_();
	}

	void Vmcs::loadRegs(HelX86VirtualizationRegs *regs) {
		regs->rax = state.rax;
		regs->rbx = state.rbx;
		regs->rcx = state.rcx;
		regs->rdx = state.rdx;
		regs->rsi = state.rsi;
		regs->rdi = state.rdi;
		regs->rbp = state.rbp;
		regs->r8 = state.r8;
		regs->r9 = state.r9;
		regs->r10 = state.r10;
		regs->r11 = state.r11;
		regs->r12 = state.r12;
		regs->r13 = state.r13;
		regs->r14 = state.r14;
		regs->r15 = state.r15;
		regs->cr2 = state.cr2;

		// See storeRegs().
		StatelessIrqLock irqLock;
		acquire_();

		regs->rsp = vmread(GUEST_RSP);
		regs->rip = vmread(GUEST_RIP);

		#define GET_SEGMENT(segment, segment_capital) \
            regs->segment.base = vmread(GUEST_##segment_capital##_BASE); \
            regs->segment.limit = vmread(GUEST_##segment_capital##_LIMIT); \
            regs->segment.selector = vmread(GUEST_##segment_capital##_SELECTOR); \
            { \
                auto seg = vmread(GUEST_##segment_capital##_ACCESS_RIGHT); \
                regs->segment.type = seg & 0xF; \
                regs->segment.s = (seg >> 4) & 1; \
                regs->segment.dpl = (seg >> 5) & 3; \
                regs->segment.present = (seg >> 7) & 1; \
                regs->segment.avl = (seg >> 12) & 1; \
                regs->segment.l = (seg >> 13) & 1; \
                regs->segment.db = (seg >> 14) & 1; \
                regs->segment.g = (seg >> 15) & 1; \
                regs->segment.unusable = (seg >> 16) & 1; \
            }

		GET_SEGMENT(cs, CS);
		GET_SEGMENT(ds, DS);
		GET_SEGMENT(ss, SS);
		GET_SEGMENT(es, ES);
		GET_SEGMENT(fs, FS);
		GET_SEGMENT(gs, GS);

		GET_SEGMENT(ldt, LDTR);
		GET_SEGMENT(tr, TR);

		regs->gdt.base = vmread(GUEST_GDTR_BASE);
		regs->gdt.limit = vmread(GUEST_GDTR_LIMIT);

		regs->idt.base = vmread(GUEST_IDTR_BASE);
		regs->idt.limit = vmread(GUEST_IDTR_LIMIT);

		regs->cr0 = vmread(GUEST_CR0);
		regs->cr3 = vmread(GUEST_CR3);
		regs->cr4 = vmread(GUEST_CR4);

		regs->efer = vmread(VMCS_FIELD_GUEST_EFER_FULL);
		release_();
	}

	void Vmcs::acquire_() {
		auto &current = currentVmcs.get();
		if(current == this)
			return;
		assert(!activeCpu_ || activeCpu_ == getCpuData());
		if(vmptrld((PhysicalAddr)region))
			infoLogger() << "vmx: VMCS load failed" << frg::endlog;
		current = this;
		activeCpu_ = getCpuData();
	}

	void Vmcs::release_() {
		if(resident_ || !activeCpu_)
			return;
		assert(activeCpu_ == getCpuData());
		vmclear((PhysicalAddr)region);
		auto &current = currentVmcs.get();
		if(current == this)
			current = nullptr;
		activeCpu_ = nullptr;
		launched_ = false;
	}

	Vmcs::~Vmcs() {
		assert(!activeCpu_);
		physicalAllocator->free((size_t)region, kPageSize);
	}
}
