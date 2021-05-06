#include <thor-internal/arch/vmx.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/arch/ept.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/thread.hpp>
#include <x86/machine.hpp>

extern "C" void vmLaunch(void* state);
extern "C" void vmResume(void* state);

namespace thor::vmx {
	bool vmxon() {
		infoLogger() << "vmx: enabling vmx" << frg::endlog;

		auto vmxonRegion = physicalAllocator->allocate(kPageSize);
		assert(reinterpret_cast<PhysicalAddr>(vmxonRegion) != static_cast<PhysicalAddr>(-1) && "OOM");

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
			infoLogger() << "vmx: processor entered vmxon operation" << frg::endlog;
		} else {
			infoLogger() << "vmx: vmxon failed" << frg::endlog;
		}

		return successful;
	}

	static inline int vmptrld(PhysicalAddr vmcs) {
		uint8_t ret;
		asm volatile (
			"vmptrld %[pa];"
			"setna %[ret];"
			: [ret]"=rm"(ret)
			: [pa]"m"(vmcs)
			: "cc", "memory");
		return ret;
	}

	static inline int vmclear(PhysicalAddr vmcs) {
		uint8_t ret;
		asm volatile (
			"vmclear %[pa];"
			"setna %[ret];"
			: [ret]"=rm"(ret)
			: [pa]"m"(vmcs)
			: "cc", "memory");
		return ret;
	}

	static inline int vmwrite(uint64_t encoding, uint64_t value) {
		uint8_t ret;
		asm volatile (
			"vmwrite %1, %2;"
			"setna %[ret]"
			: [ret]"=rm"(ret)
			: "rm"(value), "r"(encoding)
			: "cc", "memory");

		return ret;
	}

	static inline uint64_t vmread(uint64_t encoding) {
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

	Vmcs::Vmcs(smarter::shared_ptr<EptSpace> ept) : space(ept) {
		infoLogger() << "vmx: Creating VMCS" << frg::endlog;
		region = (void*)physicalAllocator->allocate(kPageSize);
		PageAccessor regionAccessor{(PhysicalAddr)region};
		memset(regionAccessor.get(), 0, kPageSize);
		uint32_t vmxRevision = common::x86::rdmsr(0x480);
		*(uint32_t*)regionAccessor.get() = static_cast<uint32_t>(vmxRevision);
		if(vmptrld((PhysicalAddr)region)) {
			infoLogger() << "vmx: VMCS load failed" << frg::endlog;
		}

		//Set up basic controls.
		uint64_t allowedPinBased = common::x86::rdmsr(MSR_IA32_VMX_PINBASED_CTLS);
		uint32_t pinbased = (uint32_t)allowedPinBased & (uint32_t)(allowedPinBased >> 32);
		vmwrite(PIN_BASED_VM_EXEC_CONTROLS, pinbased | 1);
		uint64_t allowedCpu = common::x86::rdmsr(MSR_IA32_VMX_PROCBASED_CTLS);
		uint32_t cpu = (uint32_t)allowedCpu & (uint32_t)(allowedCpu >> 32);
		vmwrite(PROC_BASED_VM_EXEC_CONTROLS,
				cpu |
				VMEXIT_ON_HLT |
				VMEXIT_ON_PIO |
				SECONDARY_CONTROLS_ON);
		vmwrite(PROC_BASED_VM_EXEC_CONTROLS2,
				EPT_ENABLE |
				UNRESTRICTED_GUEST |
				VMEXIT_ON_DESCRIPTOR);
		vmwrite(EXCEPTION_BITMAP, 0);

		uint64_t vmExitCtrls = common::x86::rdmsr(0x483);
		uint32_t vmExitCtrlsLo = (uint32_t)vmExitCtrls;
		uint32_t vmExitCtrlsHi = (uint32_t)(vmExitCtrls >> 32);
		uint32_t vm_exit_ctls = 0;
		vm_exit_ctls |= VMEXIT_CONTROLS_LONG_MODE;
		vm_exit_ctls |= VMEXIT_CONTROLS_LOAD_IA32_EFER; // Load IA32_EFER on vm-exit
		vm_exit_ctls |= vmExitCtrlsLo;
		vm_exit_ctls &= vmExitCtrlsHi;
		vmwrite(VM_EXIT_CONTROLS, vm_exit_ctls);

		uint64_t vmEntryCtrls = common::x86::rdmsr(0x484);
		uint32_t vmEntryCtrlsLo = (uint32_t)vmEntryCtrls;
		uint32_t vmEntryCtrlsHi = (uint32_t)(vmEntryCtrls >> 32);
		uint32_t vm_entry_ctls = 0;
		vm_entry_ctls |= 1 << 15;
		vm_entry_ctls |= vmEntryCtrlsLo;
		vm_entry_ctls &= vmEntryCtrlsHi;
		vmwrite(VM_ENTRY_CONTROLS, vm_entry_ctls);

		uint64_t cr0, cr4;
		asm volatile ("mov %%cr0,%0" : "=r"(cr0));
		asm volatile ("mov %%cr4,%0" : "=r"(cr4));

		//Set up host state on vmexit.
		uint32_t cr0Fixed = (uint32_t)common::x86::rdmsr(IA32_VMX_CR0_FIXED0_MSR);
		vmwrite(HOST_CR0, cr0Fixed | cr0);
		uint32_t cr4Fixed = (uint32_t)common::x86::rdmsr(IA32_VMX_CR4_FIXED0_MSR);
		vmwrite(HOST_CR4, cr4Fixed | cr4);

		common::x86::Gdtr gdtr;
		asm volatile("sgdt %[gdt]": [gdt]"=m"(gdtr));
		common::x86::Idtr idtr;
		asm volatile("sidt %[idt]": [idt]"=m"(idtr));

		uint32_t* gdt = gdtr.pointer;
		uint32_t entry1 = (gdt[kGdtIndexTask * 2] >> 16) & 0xFFFF;
		uint32_t entry2 = (gdt[kGdtIndexTask * 2 + 1]) & 0xFF;
		uint32_t entry3 = (gdt[kGdtIndexTask * 2 + 1] >> 24) & 0xFF;
		uint32_t entry4 = (gdt[kGdtIndexTask * 2 + 2]);
		uint64_t trAddr = ((uint64_t)entry4 << 32) |
			(entry1 | ((entry2) << 16) | ((entry3) << 24));


		vmwrite(HOST_TR_BASE, trAddr);
		vmwrite(HOST_GDTR_BASE, (size_t)gdtr.pointer);
		vmwrite(HOST_IDTR_BASE, (size_t)idtr.pointer);
		vmwrite(HOST_EFER_FULL, common::x86::rdmsr(0xc0000080));

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

		vmwrite(CTLS_EPTP, ept->spaceRoot | 6 | (4 - 1) << 3 | (1 << 6));
		state = {};


		if(getCpuData()->haveXsave){
			hostFstate = (uint8_t*)kernelAlloc->allocate(getCpuData()->xsaveRegionSize);
			assert(reinterpret_cast<PhysicalAddr>(hostFstate) != static_cast<PhysicalAddr>(-1) && "OOM");
			guestFstate = (uint8_t*)kernelAlloc->allocate(getCpuData()->xsaveRegionSize);
			assert(reinterpret_cast<PhysicalAddr>(guestFstate) != static_cast<PhysicalAddr>(-1) && "OOM");
			memset((void*)hostFstate, 0, getCpuData()->xsaveRegionSize);
			memset((void*)guestFstate, 0, getCpuData()->xsaveRegionSize);
		} else {
			hostFstate = (uint8_t*)kernelAlloc->allocate(512);
			hostFstate = (uint8_t*)kernelAlloc->allocate(getCpuData()->xsaveRegionSize);
			guestFstate = (uint8_t*)kernelAlloc->allocate(512);
			assert(reinterpret_cast<PhysicalAddr>(guestFstate) != static_cast<PhysicalAddr>(-1) && "OOM");
			memset((void*)hostFstate, 0, 512);
			memset((void*)guestFstate, 0, 512);
		}
	}

	HelVmexitReason Vmcs::run() {
		uint16_t es;
		uint16_t cs;
		uint16_t ss;
		uint16_t ds;
		uint16_t fs;
		uint16_t gs;
		uint16_t tr;
		uint64_t cr3;
		asm volatile ("mov %%cr3,%0" : "=r"(cr3));
		asm volatile ("str %[tr]" : [tr]"=rm"(tr));
		asm volatile ("mov %%es,%0" : "=r"(es));
		asm volatile ("mov %%cs,%0" : "=r"(cs));
		asm volatile ("mov %%ss,%0" : "=r"(ss));
		asm volatile ("mov %%ds,%0" : "=r"(ds));
		asm volatile ("mov %%fs,%0" : "=r"(fs));
		asm volatile ("mov %%gs,%0" : "=r"(gs));

		vmwrite(HOST_ES_SELECTOR, es);
		vmwrite(HOST_CS_SELECTOR, cs);
		vmwrite(HOST_SS_SELECTOR, ss);
		vmwrite(HOST_DS_SELECTOR, ds);
		vmwrite(HOST_FS_SELECTOR, fs);
		vmwrite(HOST_GS_SELECTOR, gs);
		vmwrite(HOST_TR_SELECTOR, tr);
		vmwrite(HOST_FS_BASE, common::x86::rdmsr(MSR_FS_BASE));
		vmwrite(HOST_GS_BASE, common::x86::rdmsr(MSR_GS_BASE));
		vmwrite(HOST_CR3, cr3);

		HelVmexitReason exitInfo{};

		while(1) {
			/*
			 * NOTE: this will only work as long as long
			 * as threads always stay on the same cpu,
			 * once that's changed all of the vmcs structures
			 * owned by a thread will have to be vmcleared
			 * and set launched = false;
			 */
			asm volatile("cli");
			vmclear((PhysicalAddr)region);
			vmptrld((PhysicalAddr)region);
			if(getCpuData()->haveXsave){
				common::x86::xsave((uint8_t*)hostFstate, ~0);
				common::x86::xrstor((uint8_t*)guestFstate, ~0);
			} else {
				asm volatile ("fxsaveq %0" : : "m" (*hostFstate));
				asm volatile ("fxrstorq %0" : : "m" (*guestFstate));
			}
			launched = false;
			if(!launched) {
				vmLaunch((void*)&state);
				launched = true;
			} else {
				vmResume((void*)&state);
			}

			if(getCpuData()->haveXsave){
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
			asm volatile("sti");

			auto error = vmread(VM_INSTRUCTION_ERROR);
			if(error) {
				infoLogger() << "vmx: vmx error" << error << frg::endlog;
				exitInfo.exitReason = khelVmexitError;
				return exitInfo;
			}

			auto reason = vmread(VM_EXIT_REASON);
			if(reason == VMEXIT_HLT) {
				infoLogger() << "vmx: hlt" << frg::endlog;
				exitInfo.exitReason = khelVmexitHlt;
				return exitInfo;
			} else if(reason == VMEXIT_EPT_VIOLATION) {
				size_t address = vmread(EPT_VIOLATION_ADDRESS);
				size_t exitFlags = vmread(EPT_VIOLATION_FLAGS);
				uint32_t flags = 0;
				if(exitFlags & 1)
					flags |= AddressSpace::kFaultWrite;
				if(exitFlags & (1 << 2))
					flags |= AddressSpace::kFaultExecute;

				bool handled = Thread::asyncBlockCurrent(space->handleFault(address, flags,
						getCurrentThread()->mainWorkQueue()->take()));
				if(!handled) {
					exitInfo.exitReason = khelVmexitTranslationFault;
					exitInfo.address = address;
					exitInfo.flags = exitFlags;
					return exitInfo;
				}
			}else if(reason == VMEXIT_EXTERNAL_INTERRUPT) {
				infoLogger() << "vmx: external-interrupt exit" << frg::endlog;
			}
		}
	}

	void Vmcs::storeRegs(const HelX86VirtualizationRegs *regs) {
		memcpy(&state, regs, sizeof(GuestState));

		vmwrite(GUEST_RSP, regs->rsp);
		vmwrite(GUEST_RIP, regs->rip);

		vmwrite(GUEST_CS_BASE, regs->cs.base);
		vmwrite(GUEST_CS_LIMIT, regs->cs.limit);
		vmwrite(GUEST_CS_SELECTOR, regs->cs.selector);
		vmwrite(GUEST_CS_AR_BYTES, regs->cs.ar_bytes);
		vmwrite(GUEST_CS_ACCESS_RIGHT, regs->cs.access_right);

		vmwrite(GUEST_DS_BASE, regs->ds.base);
		vmwrite(GUEST_DS_LIMIT, regs->ds.limit);
		vmwrite(GUEST_DS_SELECTOR, regs->ds.selector);
		vmwrite(GUEST_DS_AR_BYTES, regs->ds.ar_bytes);
		vmwrite(GUEST_DS_ACCESS_RIGHT, regs->ds.access_right);

		vmwrite(GUEST_ES_BASE, regs->es.base);
		vmwrite(GUEST_ES_LIMIT, regs->es.limit);
		vmwrite(GUEST_ES_SELECTOR, regs->es.selector);
		vmwrite(GUEST_ES_AR_BYTES, regs->es.ar_bytes);
		vmwrite(GUEST_ES_ACCESS_RIGHT, regs->es.access_right);

		vmwrite(GUEST_FS_BASE, regs->fs.base);
		vmwrite(GUEST_FS_LIMIT, regs->fs.limit);
		vmwrite(GUEST_FS_SELECTOR, regs->fs.selector);
		vmwrite(GUEST_FS_AR_BYTES, regs->fs.ar_bytes);
		vmwrite(GUEST_FS_ACCESS_RIGHT, regs->fs.access_right);

		vmwrite(GUEST_GS_BASE, regs->gs.base);
		vmwrite(GUEST_GS_LIMIT, regs->gs.limit);
		vmwrite(GUEST_GS_SELECTOR, regs->gs.selector);
		vmwrite(GUEST_GS_AR_BYTES, regs->gs.ar_bytes);
		vmwrite(GUEST_GS_ACCESS_RIGHT, regs->gs.access_right);

		vmwrite(GUEST_SS_BASE, regs->ss.base);
		vmwrite(GUEST_SS_LIMIT, regs->ss.limit);
		vmwrite(GUEST_SS_SELECTOR, regs->ss.selector);
		vmwrite(GUEST_SS_AR_BYTES, regs->ss.ar_bytes);
		vmwrite(GUEST_SS_ACCESS_RIGHT, regs->ss.access_right);

		vmwrite(GUEST_TR_BASE, regs->tr.base);
		vmwrite(GUEST_TR_SELECTOR, regs->tr.selector);
		vmwrite(GUEST_TR_LIMIT, regs->tr.limit);
		vmwrite(GUEST_TR_AR_BYTES, regs->ldt.ar_bytes);
		vmwrite(GUEST_TR_ACCESS_RIGHT, regs->tr.access_right);

		vmwrite(GUEST_LDTR_BASE, regs->ldt.base);
		vmwrite(GUEST_LDTR_SELECTOR, regs->ldt.selector);
		vmwrite(GUEST_LDTR_LIMIT, regs->ldt.limit);
		vmwrite(GUEST_LDTR_AR_BYTES, regs->ldt.ar_bytes);
		vmwrite(GUEST_LDTR_ACCESS_RIGHT, regs->ldt.access_right);

		vmwrite(GUEST_CR0, regs->cr0);
		vmwrite(GUEST_CR3, regs->cr3);
		vmwrite(GUEST_CR4, regs->cr4);

		vmwrite(VMCS_FIELD_GUEST_EFER_FULL, regs->efer);
	}

	void Vmcs::loadRegs(HelX86VirtualizationRegs *res) {
		memcpy(res, &state, sizeof(GuestState));

		res->rsp = vmread(GUEST_RSP);
		res->rip = vmread(GUEST_RIP);

		res->cs.base = vmread(GUEST_CS_BASE);
		res->cs.limit = vmread(GUEST_CS_LIMIT);
		res->cs.selector = vmread(GUEST_CS_SELECTOR);
		res->cs.ar_bytes = vmread(GUEST_CS_AR_BYTES);
		res->cs.access_right = vmread(GUEST_CS_ACCESS_RIGHT);

		res->ds.base = vmread(GUEST_DS_BASE);
		res->ds.limit = vmread(GUEST_DS_LIMIT);
		res->ds.selector = vmread(GUEST_DS_SELECTOR);
		res->ds.ar_bytes = vmread(GUEST_DS_AR_BYTES);
		res->ds.access_right = vmread(GUEST_DS_ACCESS_RIGHT);

		res->es.base = vmread(GUEST_ES_BASE);
		res->es.limit = vmread(GUEST_ES_LIMIT);
		res->es.selector = vmread(GUEST_ES_SELECTOR);
		res->es.ar_bytes = vmread(GUEST_ES_AR_BYTES);
		res->es.access_right = vmread(GUEST_ES_ACCESS_RIGHT);

		res->fs.base = vmread(GUEST_FS_BASE);
		res->fs.limit = vmread(GUEST_FS_LIMIT);
		res->fs.selector = vmread(GUEST_FS_SELECTOR);
		res->fs.ar_bytes = vmread(GUEST_FS_AR_BYTES);
		res->fs.access_right = vmread(GUEST_FS_ACCESS_RIGHT);

		res->gs.base = vmread(GUEST_GS_BASE);
		res->gs.limit = vmread(GUEST_GS_LIMIT);
		res->gs.selector = vmread(GUEST_GS_SELECTOR);
		res->gs.ar_bytes = vmread(GUEST_GS_AR_BYTES);
		res->gs.access_right = vmread(GUEST_GS_ACCESS_RIGHT);

		res->ss.base = vmread(GUEST_SS_BASE);
		res->ss.limit = vmread(GUEST_SS_LIMIT);
		res->ss.selector = vmread(GUEST_SS_SELECTOR);
		res->ss.ar_bytes = vmread(GUEST_SS_AR_BYTES);
		res->ss.access_right = vmread(GUEST_SS_ACCESS_RIGHT);

		res->tr.base = vmread(GUEST_TR_BASE);
		res->tr.limit = vmread(GUEST_TR_LIMIT);
		res->tr.selector = vmread(GUEST_TR_SELECTOR);
		res->tr.ar_bytes = vmread(GUEST_TR_AR_BYTES);
		res->tr.access_right = vmread(GUEST_TR_ACCESS_RIGHT);

		res->ldt.base = vmread(GUEST_LDTR_BASE);
		res->ldt.limit = vmread(GUEST_LDTR_LIMIT);
		res->ldt.selector = vmread(GUEST_LDTR_SELECTOR);
		res->ldt.ar_bytes = vmread(GUEST_LDTR_AR_BYTES);
		res->ldt.access_right = vmread(GUEST_LDTR_ACCESS_RIGHT);

		res->cr0 = vmread(GUEST_CR0);
		res->cr3 = vmread(GUEST_CR3);
		res->cr4 = vmread(GUEST_CR4);

		res->efer = vmread(VMCS_FIELD_GUEST_EFER_FULL);
	}

	Vmcs::~Vmcs() {
		physicalAllocator->free((size_t)region, kPageSize);
	}
}
