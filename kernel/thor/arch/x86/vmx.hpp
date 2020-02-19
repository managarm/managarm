#pragma once

#include <arch/x86/ept.hpp>
#include <generic/virtualization.hpp>
#include "../../../hel/include/hel.h"

namespace thor::vmx {
	constexpr uint64_t MSR_IA32_VMX_PINBASED_CTLS        = 0x00000481;
	constexpr uint64_t PIN_BASED_VM_EXEC_CONTROLS        = 0x00004000;
	constexpr uint64_t MSR_IA32_VMX_PROCBASED_CTLS       = 0x00000482;
	constexpr uint64_t PROC_BASED_VM_EXEC_CONTROLS       = 0x00004002;
	constexpr uint64_t PROC_BASED_VM_EXEC_CONTROLS2      = 0x0000401e;
	constexpr uint64_t EXCEPTION_BITMAP                  = 0x00004004;
	constexpr uint64_t VM_EXIT_CONTROLS                  = 0x0000400c;
	constexpr uint64_t MSR_IA32_VMX_EXIT_CTLS            = 0x00000483;
	constexpr uint64_t VM_EXIT_HOST_ADDR_SPACE_SIZE      = 0x00000200;
	constexpr uint64_t VM_ENTRY_CONTROLS                 = 0x00004012;
	constexpr uint64_t MSR_IA32_VMX_ENTRY_CTLS           = 0x00000484;
	constexpr uint64_t VM_ENTRY_IA32E_MODE               = 0x00000200;
	constexpr uint64_t HOST_CR0                          = 0x00006c00;
	constexpr uint64_t HOST_CR3                          = 0x00006c02;
	constexpr uint64_t HOST_CR4                          = 0x00006c04;
	constexpr uint64_t HOST_ES_SELECTOR                  = 0x00000c00;
	constexpr uint64_t HOST_CS_SELECTOR                  = 0x00000c02;
	constexpr uint64_t HOST_SS_SELECTOR                  = 0x00000c04;
	constexpr uint64_t HOST_DS_SELECTOR                  = 0x00000c06;
	constexpr uint64_t HOST_FS_SELECTOR                  = 0x00000c08;
	constexpr uint64_t HOST_GS_SELECTOR                  = 0x00000c0a;
	constexpr uint64_t HOST_TR_SELECTOR                  = 0x00000c0c;
	constexpr uint64_t HOST_FS_BASE                      = 0x00006c06;
	constexpr uint64_t HOST_GS_BASE                      = 0x00006c08;
	constexpr uint64_t HOST_TR_BASE                      = 0x00006c0a;
	constexpr uint64_t HOST_GDTR_BASE                    = 0x00006c0c;
	constexpr uint64_t HOST_IDTR_BASE                    = 0x00006c0e;
	constexpr uint64_t HOST_IA32_SYSENTER_ESP            = 0x00006c10;
	constexpr uint64_t HOST_IA32_SYSENTER_EIP            = 0x00006c12;
	constexpr uint64_t HOST_IA32_SYSENTER_CS             = 0x00004c00;
	constexpr uint64_t HOST_RSP                          = 0x00006c14;
	constexpr uint64_t HOST_RIP                          = 0x00006c16;
	constexpr uint64_t RFLAG_RESERVED                    = (1 << 1);
	constexpr uint64_t GUEST_RFLAG                       = 0x00006820;
	constexpr uint64_t HOST_GDT_LIMIT                    = 14 * 8;

	constexpr uint64_t IA32_VMX_BASIC_MSR                = 0x480;
	constexpr uint64_t IA32_VMX_CR0_FIXED0_MSR           = 0x486;
	constexpr uint64_t IA32_VMX_CR0_FIXED1_MSR           = 0x487;
	constexpr uint64_t IA32_VMX_CR4_FIXED0_MSR           = 0x488;
	constexpr uint64_t IA32_VMX_CR4_FIXED1_MSR           = 0x489;
	constexpr uint64_t IA32_VMX_PINBASED_CTLS_MSR        = 0x481;
	constexpr uint64_t IA32_VMX_PRI_PROCBASED_CTLS_MSR   = 0x482;
	constexpr uint64_t IA32_VMX_SEC_PROCBASED_CTLS_MSR   = 0x48b;
	constexpr uint64_t IA32_VMX_EPT_VPID_CAP_MSR         = 0x48c;
	constexpr uint64_t IA32_VMX_VM_EXIT_CTLS_MSR         = 0x483;
	constexpr uint64_t IA32_VMX_VM_ENTRY_CTLS_MSR        = 0x484;
	constexpr uint64_t HOST_EFER_FULL                    = 0x00002c02;

	constexpr uint64_t GUEST_DR7 = 0x0000681A;
	constexpr uint64_t GUEST_RSP = 0x0000681C;
	constexpr uint64_t GUEST_RIP = 0x0000681E;
	constexpr uint64_t GUEST_CR0 = 0x00006800;
	constexpr uint64_t GUEST_CR3 = 0x00006802;
	constexpr uint64_t GUEST_CR4 = 0x00006804;
	constexpr uint64_t CTLS_EPTP = 0x0000201A;

	constexpr uint64_t GUEST_ES_SELECTOR                = 0x00000800;
	constexpr uint64_t GUEST_CS_SELECTOR                = 0x00000802;
	constexpr uint64_t GUEST_SS_SELECTOR                = 0x00000804;
	constexpr uint64_t GUEST_DS_SELECTOR                = 0x00000806;
	constexpr uint64_t GUEST_FS_SELECTOR                = 0x00000808;
	constexpr uint64_t GUEST_GS_SELECTOR                = 0x0000080a;
	constexpr uint64_t GUEST_LDTR_SELECTOR              = 0x0000080c;
	constexpr uint64_t GUEST_TR_SELECTOR                = 0x0000080e;
	constexpr uint64_t GUEST_ES_LIMIT                   = 0x00004800;
	constexpr uint64_t GUEST_CS_LIMIT                   = 0x00004802;
	constexpr uint64_t GUEST_SS_LIMIT                   = 0x00004804;
	constexpr uint64_t GUEST_DS_LIMIT                   = 0x00004806;
	constexpr uint64_t GUEST_FS_LIMIT                   = 0x00004808;
	constexpr uint64_t GUEST_GS_LIMIT                   = 0x0000480a;
	constexpr uint64_t GUEST_LDTR_LIMIT                 = 0x0000480c;
	constexpr uint64_t GUEST_TR_LIMIT                   = 0x0000480e;
	constexpr uint64_t GUEST_GDTR_LIMIT                 = 0x00004810;
	constexpr uint64_t GUEST_IDTR_LIMIT                 = 0x00004812;
	constexpr uint64_t GUEST_ES_AR_BYTES                = 0x00004814;
	constexpr uint64_t GUEST_CS_AR_BYTES                = 0x00004816;
	constexpr uint64_t GUEST_SS_AR_BYTES                = 0x00004818;
	constexpr uint64_t GUEST_DS_AR_BYTES                = 0x0000481a;
	constexpr uint64_t GUEST_FS_AR_BYTES                = 0x0000481c;
	constexpr uint64_t GUEST_GS_AR_BYTES                = 0x0000481e;
	constexpr uint64_t GUEST_LDTR_AR_BYTES              = 0x00004820;
	constexpr uint64_t GUEST_TR_AR_BYTES                = 0x00004822;
	constexpr uint64_t GUEST_ES_BASE                    = 0x00006806;
	constexpr uint64_t GUEST_CS_BASE                    = 0x00006808;
	constexpr uint64_t GUEST_SS_BASE                    = 0x0000680a;
	constexpr uint64_t GUEST_DS_BASE                    = 0x0000680c;
	constexpr uint64_t GUEST_FS_BASE                    = 0x0000680e;
	constexpr uint64_t GUEST_GS_BASE                    = 0x00006810;
	constexpr uint64_t GUEST_LDTR_BASE                  = 0x00006812;
	constexpr uint64_t GUEST_TR_BASE                    = 0x00006814;
	constexpr uint64_t GUEST_GDTR_BASE                  = 0x00006816;
	constexpr uint64_t GUEST_IDTR_BASE                  = 0x00006818;

	constexpr uint64_t GUEST_ES_ACCESS_RIGHT        = 0x00004814;
	constexpr uint64_t GUEST_CS_ACCESS_RIGHT        = 0x00004816;
	constexpr uint64_t GUEST_SS_ACCESS_RIGHT        = 0x00004818;
	constexpr uint64_t GUEST_DS_ACCESS_RIGHT        = 0x0000481A;
	constexpr uint64_t GUEST_FS_ACCESS_RIGHT        = 0x0000481C;
	constexpr uint64_t GUEST_GS_ACCESS_RIGHT        = 0x0000481E;
	constexpr uint64_t GUEST_LDTR_ACCESS_RIGHT      = 0x00004820;
	constexpr uint64_t GUEST_TR_ACCESS_RIGHT        = 0x00004822;
	constexpr uint64_t GUEST_INTERRUPTIBILITY_STATE = 0x00004824;
	constexpr uint64_t GUEST_SMBASE                 = 0x00004828;
	constexpr uint64_t GUEST_IA32_SYSENTER_CS       = 0x0000482A;
	constexpr uint64_t GUEST_VMX_PREEMPTION_TIMER   = 0x0000482E;
	constexpr uint64_t VMCS_FIELD_GUEST_EFER_FULL   = 0x00002806;
	constexpr uint64_t MSR_FS_BASE                  = 0xc0000100;
	constexpr uint64_t MSR_GS_BASE                  = 0xc0000101;
	constexpr uint64_t EFER                         = 0xc0000080;

	constexpr uint64_t GUEST_ACTIVITY_STATE              = 0X00004826;
	constexpr uint64_t VMX_PREEMPTION_TIMER_VALUE        = 0x0000482E;
	constexpr uint64_t VMCS_LINK_POINTER                 = 0x00002800;
	constexpr uint64_t GUEST_INTR_STATUS                 = 0x00000810;
	constexpr uint64_t GUEST_PML_INDEX                   = 0x00000812;
	constexpr uint64_t VM_EXIT_REASON                    = 0x00004402;
	constexpr uint64_t VM_INSTRUCTION_ERROR              = 0x00004400;

	constexpr uint64_t DATA_ACCESS_RIGHT = (0x3 | 1 << 4 | 1 << 7);
	constexpr uint64_t CODE_ACCESS_RIGHT = (0x3 | 1 << 4 | 1 << 7 | 1 << 13);
	constexpr uint64_t LDTR_ACCESS_RIGHT = (0x2 | 1 << 7);
	constexpr uint64_t TR_ACCESS_RIGHT   = (0x3 | 1 << 7);

	constexpr uint64_t VMEXIT_EXTERNAL_INTERRUPT = 1;
	constexpr uint64_t VMEXIT_HLT                = 12;

	constexpr uint64_t VMEXIT_CONTROLS_LONG_MODE      = 1 << 9;
	constexpr uint64_t VMEXIT_CONTROLS_LOAD_IA32_EFER = 1 << 21;
	constexpr uint64_t VMEXIT_ON_HLT                  = 1 << 7;
	constexpr uint64_t VMEXIT_ON_PIO                  = 1 << 24;
	constexpr uint64_t SECONDARY_CONTROLS_ON          = 1 << 31;
	constexpr uint64_t EPT_ENABLE                     = 1 << 1;
	constexpr uint64_t UNRESTRICTED_GUEST             = 1 << 7;
	constexpr uint64_t VMEXIT_ON_DESCRIPTOR           = 1 << 2;


	bool vmxon();

	struct Vmcs : VirtualizedCpu {
		Vmcs(smarter::shared_ptr<EptSpace> ept);
		Vmcs(const Vmcs& vmcs) = delete;
		Vmcs& operator=(const Vmcs& vmcs) = delete;
		HelVmexitReason run();
		void storeRegs(const HelX86VirtualizationRegs *regs);
		void loadRegs(HelX86VirtualizationRegs *res);
		~Vmcs();
		void *region;
		uint8_t* hostFstate;
		uint8_t* guestFstate;

		smarter::shared_ptr<EptSpace> space;
		bool launched = false;

		GuestState state;
	};
}
