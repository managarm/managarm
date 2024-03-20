#pragma once

#include <hel.h>
#include <thor-internal/arch/npt.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor::svm {
	bool init();

	struct [[gnu::packed]] Vmcb {
		uint16_t iceptCrReads;
		uint16_t iceptCrWrites;
		uint16_t iceptDrReads;
		uint16_t iceptDrWrites;
		uint32_t iceptExceptions;

		uint32_t iceptIntr : 1;
		uint32_t iceptNmi : 1;
		uint32_t iceptSmi : 1;
		uint32_t iceptInit : 1;
		uint32_t iceptVintr : 1;
		uint32_t iceptCr0Writes : 1;
		uint32_t iceptIdtrReads : 1;
		uint32_t iceptGdtrReads : 1;
		uint32_t iceptLdtrReads : 1;
		uint32_t iceptTrReads : 1;
		uint32_t iceptTdtrWrites : 1;
		uint32_t iceptGdtrWrites : 1;
		uint32_t iceptLdtrWrites : 1;
		uint32_t iceptTrWrites : 1;
		uint32_t iceptRdtsc : 1;
		uint32_t iceptRdpmc : 1;
		uint32_t iceptPushf : 1;
		uint32_t iceptPopf : 1;
		uint32_t iceptCpuid : 1;
		uint32_t iceptRsm : 1;
		uint32_t iceptIret : 1;
		uint32_t iceptInt : 1;
		uint32_t iceptInvd : 1;
		uint32_t iceptPause : 1;
		uint32_t iceptHlt : 1;
		uint32_t iceptInvlpg : 1;
		uint32_t iceptInvlpga : 1;
		uint32_t iceptIo : 1;
		uint32_t iceptMsr : 1;
		uint32_t iceptTaskSwitch : 1;
		uint32_t ferrFreeze : 1;
		uint32_t iceptShutdown : 1;

		uint32_t iceptVmrun : 1;
		uint32_t iceptVmmcall : 1;
		uint32_t iceptVmload : 1;
		uint32_t iceptVmsave : 1;
		uint32_t iceptStgi : 1;
		uint32_t iceptClgi : 1;
		uint32_t iceptSkinit : 1;
		uint32_t iceptRdtscp : 1;
		uint32_t iceptIcebp : 1;
		uint32_t iceptWbinvd : 1;
		uint32_t iceptMonitor : 1;
		uint32_t iceptMwaitUnconditional : 1;
		uint32_t iceptMwaitIfArmed : 1;
		uint32_t iceptXsetbv : 1;
		uint32_t iceptRdpru : 1;
		uint32_t iceptEferWrite : 1;
		uint32_t iceptCrWritesAfterFinish : 16;

		uint32_t iceptAllIvlpgb : 1;
		uint32_t iceptIllegalInvlpgb : 1;
		uint32_t iceptPcid : 1;
		uint32_t iceptMcommit : 1;
		uint32_t iceptTlbsync : 1;
		uint32_t reserved : 27;

		uint8_t reserved0[0x24];
		uint16_t pauseFilterThreshold;
		uint16_t pauseFilterCount;

		uint64_t iopmBasePa;
		uint64_t msrpmBasePa;
		uint64_t tscOffset;

		uint64_t guestAsid : 32;
		uint64_t tlbControl : 8;
		uint64_t reserved1 : 24;

		uint64_t vTpr : 8;
		uint64_t vIrq : 1;
		uint64_t vGif : 1;
		uint64_t reserved2 : 6;
		uint64_t vIntrPriority : 4;
		uint64_t vIgnoreTpr : 1;
		uint64_t reserved3 : 3;
		uint64_t vIntMasking : 1;
		uint64_t virtualGifEnable : 1;
		uint64_t reserved4 : 5;
		uint64_t avicEnable : 1;
		uint64_t vIntrVector : 8;
		uint64_t reserved5 : 24;

		uint64_t irqShadow : 1;
		uint64_t guestIRQFlag : 1;
		uint64_t reserved6 : 62;

		uint64_t exitcode;
		uint64_t exitinfo1;
		uint64_t exitinfo2;
		uint64_t exitintinfo;

		uint64_t nptEnable : 1;
		uint64_t sevEnable : 1;
		uint64_t sevEncryptEnable : 1;
		uint64_t sssCheckEnable : 1;
		uint64_t vteEnable : 1;
		uint64_t invlpgbEnable : 1;
		uint64_t reserved7 : 56;

		uint64_t avicBar;
		uint64_t guestGhcb;
		uint64_t eventInject;
		uint64_t nptCr3;

		uint64_t lbrEnable : 1;
		uint64_t virtualVmsaveEnable : 1;
		uint64_t reserved8 : 62;

		uint32_t vmcbClean;
		uint32_t reserved9;

		uint64_t nextRip;
		uint8_t instructionLen;
		uint8_t instructionBytes[15];

		uint64_t avicBackingPage;
		uint64_t reserved10;
		uint64_t avicLogicalTable;
		uint64_t avicPhysicalTable;
		uint64_t reserved11;
		uint64_t vmsaPointer;
		
		uint8_t reserved12[0x320 - 6 * 8];

		struct [[gnu::packed]] Segment {
			uint16_t selector;
			uint16_t attrib;
			uint32_t limit;
			uint64_t base;
		};

		Segment es, cs, ss, ds, fs, gs;
		Segment gdt, ldt, idt, tr;

		uint8_t reserved13[0x2B];
		uint8_t cpl;
		uint32_t reserved14;
		uint64_t efer;
		uint8_t reserved15[0x70];
		uint64_t cr4, cr3, cr0, dr7, dr6, rflags, rip;
		uint8_t reserved16[0x58];
		uint64_t rsp;
		uint8_t reserved17[0x18];
		uint64_t rax, star, lstar, cstar, sfmask, kernel_gs_base, sysenter_cs, sysenter_esp, sysenter_eip;
		uint64_t cr2;
		uint8_t reserved18[0x20];
		uint64_t pat;
		uint64_t debugControl;
		uint64_t brFrom, brTo;
		uint64_t intFrom, intTo;
		uint8_t reserved19[0x968];
	};
	static_assert(sizeof(Vmcb) == 0x1000);

	constexpr size_t IOPM_BITMAP_SIZE = 4 * kPageSize;
	constexpr size_t MSRPM_BITMAP_SIZE = 2 * kPageSize;

	// ACCESSED FROM ASSEMBLY, DO NOT CHANGE
	struct [[gnu::packed]] GprState {
		uint64_t rbx;
		uint64_t rcx;
		uint64_t rdx;
		uint64_t rsi;
		uint64_t rdi;
		uint64_t rbp;
		uint64_t r8;
		uint64_t r9;
		uint64_t r10;
		uint64_t r11;
		uint64_t r12;
		uint64_t r13;
		uint64_t r14;
		uint64_t r15;
		uint64_t cr2;

		uint64_t dr0, dr1, dr2, dr3;
	};

	enum {
		kSvmExitHlt = 0x78,
		kSvmExitNPTFault = 0x400,
	};

	struct Vcpu final : VirtualizedCpu {
		Vcpu(smarter::shared_ptr<NptSpace> npt);
		~Vcpu();
		
		Vcpu(const Vcpu &) = delete;
		Vcpu &operator=(const Vcpu &) = delete;

		HelVmexitReason run();
		void storeRegs(const HelX86VirtualizationRegs *regs);
		void loadRegs(HelX86VirtualizationRegs *res);

		PhysicalAddr vmcb_region, host_additional_save_region, iopm_bitmap, msrpm_bitmap;
		volatile Vmcb *vmcb;
		GprState gprState;

		uint8_t *host_fpu_state, *guest_fpu_state;

		smarter::shared_ptr<NptSpace> space;
	};
}