#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include <frg/tuple.hpp>
#include <x86/gdt.hpp>
#include <x86/idt.hpp>
#include <x86/machine.hpp>
#include <x86/tss.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/kernel-stack.hpp>

namespace thor {

// --------------------------------------------------------
// Global runtime functions
// --------------------------------------------------------

enum {
	kGdtIndexNull = 0,

	kGdtIndexInitialCode = 1,

	// note that the TSS consumes two entries in the GDT.
	// we put it into the second GDT entry so that it is properly aligned.
	kGdtIndexTask = 2,

	kGdtIndexSystemIrqCode = 4,

	kGdtIndexExecutorFaultCode = 5,
	// the order of the following segments should not change
	// because syscall/sysret demands this layout
	kGdtIndexExecutorSyscallCode = 6,
	kGdtIndexExecutorKernelData = 7,
	kGdtIndexClientUserCompat = 8,
	kGdtIndexClientUserData = 9,
	kGdtIndexClientUserCode = 10,
	kGdtIndexSystemIdleCode = 11,
	kGdtIndexSystemFiberCode = 12,

	kGdtIndexSystemNmiCode = 13
};

constexpr uint16_t selectorFor(uint16_t segment, uint16_t rpl) {
	return (segment << 3) | rpl;
}

enum {
	kSelInitialCode = selectorFor(kGdtIndexInitialCode, 0),

	kSelTask = selectorFor(kGdtIndexTask, 0),
	kSelSystemIrqCode = selectorFor(kGdtIndexSystemIrqCode, 0),

	kSelExecutorFaultCode = selectorFor(kGdtIndexExecutorFaultCode, 0),
	kSelExecutorSyscallCode = selectorFor(kGdtIndexExecutorSyscallCode, 0),
	kSelExecutorKernelData = selectorFor(kGdtIndexExecutorKernelData, 0),
	kSelClientUserCompat = selectorFor(kGdtIndexClientUserCompat, 3),
	kSelClientUserData = selectorFor(kGdtIndexClientUserData, 3),
	kSelClientUserCode = selectorFor(kGdtIndexClientUserCode, 3),
	kSelSystemIdleCode = selectorFor(kGdtIndexSystemIdleCode, 0),
	kSelSystemFiberCode = selectorFor(kGdtIndexSystemFiberCode, 0),

	kSelSystemNmiCode = selectorFor(kGdtIndexSystemNmiCode, 0)
};

struct Executor;

struct Continuation {
	void *sp;
};

struct FaultImageAccessor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);

	Word *ip() { return &_frame()->rip; }
	Word *sp() { return &_frame()->rsp; }
	Word *cs() { return &_frame()->cs; }
	Word *ss() { return &_frame()->ss; }
	Word *rflags() { return &_frame()->rflags; }
	Word *code() { return &_frame()->code; }

	bool inKernelDomain() {
		if(*cs() == kSelSystemIrqCode
				|| *cs() == kSelSystemIdleCode
				|| *cs() == kSelSystemFiberCode
				|| *cs() == kSelExecutorFaultCode
				|| *cs() == kSelExecutorSyscallCode) {
			return true;
		}else{
			assert(*cs() == kSelClientUserCompat
					|| *cs() == kSelClientUserCode);
			return false;
		}
	}

	bool allowUserPages();

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct Frame {
		Word rax;
		Word rbx;
		Word rcx;
		Word rdx;
		Word rdi;
		Word rsi;
		Word r8;
		Word r9;
		Word r10;
		Word r11;
		Word r12;
		Word r13;
		Word r14;
		Word r15;
		Word rbp;
		Word code;

		// the following fields are pushed by interrupt
		Word rip;
		Word cs;
		Word rflags;
		Word rsp;
		Word ss;
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct IrqImageAccessor {
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);

	Word *ip() { return &_frame()->rip; }

	// TODO: These are only exposed for debugging.
	Word *cs() { return &_frame()->cs; }
	Word *rflags() { return &_frame()->rflags; }
	Word *ss() { return &_frame()->ss; }

	bool inPreemptibleDomain() {
		assert(*cs() == kSelSystemIdleCode
				|| *cs() == kSelSystemFiberCode
				|| *cs() == kSelExecutorFaultCode
				|| *cs() == kSelExecutorSyscallCode
				|| *cs() == kSelClientUserCompat
				|| *cs() == kSelClientUserCode);
		return true;
	}

	bool inThreadDomain() {
		assert(inPreemptibleDomain());
		if(*cs() == kSelExecutorFaultCode
				|| *cs() == kSelExecutorSyscallCode
				|| *cs() == kSelClientUserCompat
				|| *cs() == kSelClientUserCode) {
			return true;
		}else{
			return false;
		}
	}

	bool inManipulableDomain() {
		assert(inThreadDomain());
		if(*cs() == kSelClientUserCompat
				|| *cs() == kSelClientUserCode) {
			return true;
		}else{
			return false;
		}
	}

	bool inFiberDomain() {
		assert(inPreemptibleDomain());
		return *cs() == kSelSystemFiberCode;
	}

	bool inIdleDomain() {
		assert(inPreemptibleDomain());
		return *cs() == kSelSystemIdleCode;
	}

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct Frame {
		Word rax;
		Word rbx;
		Word rcx;
		Word rdx;
		Word rdi;
		Word rsi;
		Word r8;
		Word r9;
		Word r10;
		Word r11;
		Word r12;
		Word r13;
		Word r14;
		Word r15;
		Word rbp;

		// the following fields are pushed by interrupt
		Word rip;
		Word cs;
		Word rflags;
		Word rsp;
		Word ss;
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct SyscallImageAccessor {
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

	Word *number() { return &_frame()->rdi; }
	Word *in0() { return &_frame()->rsi; }
	Word *in1() { return &_frame()->rdx; }
	Word *in2() { return &_frame()->rax; }
	Word *in3() { return &_frame()->r8; }
	Word *in4() { return &_frame()->r9; }
	Word *in5() { return &_frame()->r10; }
	Word *in6() { return &_frame()->r12; }
	Word *in7() { return &_frame()->r13; }
	Word *in8() { return &_frame()->r14; }

	Word *error() { return &_frame()->rdi; }
	Word *out0() { return &_frame()->rsi; }
	Word *out1() { return &_frame()->rdx; }

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	// this struct is accessed from assembly.
	// do not randomly change its contents.
	struct Frame {
		Word rdi;
		Word rsi;
		Word rdx;
		Word rax;
		Word r8;
		Word r9;
		Word r10;
		Word r12;
		Word r13;
		Word r14;
		Word r15;
		Word rbp;
		Word rsp;
		Word rip;
		Word rflags;
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct NmiImageAccessor {
	void **expectedGs() {
		return &_frame()->expectedGs;
	}

	Word *ip() { return &_frame()->rip; }
	Word *cs() { return &_frame()->cs; }
	Word *rflags() { return &_frame()->rflags; }

private:
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct Frame {
		Word rax;
		Word rbx;
		Word rcx;
		Word rdx;
		Word rdi;
		Word rsi;
		Word r8;
		Word r9;
		Word r10;
		Word r11;
		Word r12;
		Word r13;
		Word r14;
		Word r15;
		Word rbp;

		// the following fields are pushed by interrupt
		Word rip;
		Word cs;
		Word rflags;
		Word rsp;
		Word ss;

		void *expectedGs;
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

// CpuData is some high-level struct that inherits from PlatformCpuData.
struct CpuData;

struct AbiParameters {
	uintptr_t ip;
	uintptr_t sp;
	uintptr_t argument;
};

struct UserContext {
	static void deactivate();

	UserContext();

	UserContext(const UserContext &other) = delete;

	UserContext &operator= (const UserContext &other) = delete;

	void enableIoPort(uintptr_t port);

	// Migrates this UserContext to a different CPU.
	void migrate(CpuData *cpu_data);

	// TODO: This should be private.
	UniqueKernelStack kernelStack;
	common::x86::Tss64 tss;
};

struct FiberContext {
	FiberContext(UniqueKernelStack stack);

	FiberContext(const FiberContext &other) = delete;

	FiberContext &operator= (const FiberContext &other) = delete;

	// TODO: This should be private.
	UniqueKernelStack stack;
};

struct Executor;

// Restores the current executor from its saved image.
// This is functions does the heavy lifting during task switch.
// Note: due to the attribute, this must be declared before the friend declaration below.
[[noreturn]] void restoreExecutor(Executor *executor);

struct Executor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);
	friend void workOnExecutor(Executor *executor);
	friend void restoreExecutor(Executor *executor);

	static size_t determineSize();
	static size_t determineSimdSize();

	Executor();

	explicit Executor(UserContext *context, AbiParameters abi);
	explicit Executor(FiberContext *context, AbiParameters abi);

	Executor(const Executor &other) = delete;

	~Executor();

	Executor &operator= (const Executor &other) = delete;

	void *getSyscallStack() {
		return _syscallStack;
	}

	// FIXME: remove or refactor the rdi / rflags accessors
	// as they are platform specific and need to be abstracted here
	Word *rflags() { return &general()->rflags; }

	Word *ip() { return &general()->rip; }
	Word *sp() { return &general()->rsp; }
	Word *cs() { return &general()->cs; }
	Word *ss() { return &general()->ss; }

	Word *arg0() { return &general()->rsi; }
	Word *arg1() { return &general()->rdx; }
	Word *result0() { return &general()->rdi; }
	Word *result1() { return &general()->rsi; }

private:
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct General {
		Word rax;			// offset 0x00
		Word rbx;			// offset 0x08
		Word rcx;			// offset 0x10
		Word rdx;			// offset 0x18
		Word rsi;			// offset 0x20
		Word rdi;			// offset 0x28
		Word rbp;			// offset 0x30

		Word r8;			// offset 0x38
		Word r9;			// offset 0x40
		Word r10;			// offset 0x48
		Word r11;			// offset 0x50
		Word r12;			// offset 0x58
		Word r13;			// offset 0x60
		Word r14;			// offset 0x68
		Word r15;			// offset 0x70

		Word rip;			// offset 0x78
		Word cs;			// offset 0x80
		Word rflags;		// offset 0x88
		Word rsp;			// offset 0x90
		Word ss;			// offset 0x98
		Word clientFs;		// offset 0xA0
		Word clientGs;		// offset 0xA8
	};
	static_assert(sizeof(General) == 0xB0, "Bad sizeof(General)");

	struct FxState {
		uint16_t fcw; // x87 control word
		uint16_t fsw; // x87 status word
		uint8_t ftw; // x87 tag word
		uint8_t reserved0;
		uint16_t fop;
		uint64_t fpuIp;
		uint64_t fpuDp;
		uint32_t mxcsr;
		uint32_t mxcsrMask;
		uint8_t st0[10];
		uint8_t reserved1[6];
		uint8_t st1[10];
		uint8_t reserved2[6];
		uint8_t st2[10];
		uint8_t reserved3[6];
		uint8_t st3[10];
		uint8_t reserved4[6];
		uint8_t st4[10];
		uint8_t reserved5[6];
		uint8_t st5[10];
		uint8_t reserved6[6];
		uint8_t st6[10];
		uint8_t reserved7[6];
		uint8_t st7[10];
		uint8_t reserved8[6];
		uint8_t xmm0[16];
		uint8_t xmm1[16];
		uint8_t xmm2[16];
		uint8_t xmm3[16];
		uint8_t xmm4[16];
		uint8_t xmm5[16];
		uint8_t xmm6[16];
		uint8_t xmm7[16];
		uint8_t xmm8[16];
		uint8_t xmm9[16];
		uint8_t xmm10[16];
		uint8_t xmm11[16];
		uint8_t xmm12[16];
		uint8_t xmm13[16];
		uint8_t xmm14[16];
		uint8_t xmm15[16];
		uint8_t reserved9[48];
		uint8_t available[48];
	};
	static_assert(sizeof(FxState) == 512, "Bad sizeof(FxState)");
public:
	General *general() {
		return reinterpret_cast<General *>(_pointer);
	}

	FxState *_fxState() {
		// fxState is offset from General by 0x10 bytes to make it aligned
		return reinterpret_cast<FxState *>(_pointer + sizeof(General) + 0x10);
	}

private:
	char *_pointer;
	void *_syscallStack;
	common::x86::Tss64 *_tss;
};

void saveExecutor(Executor *executor, FaultImageAccessor accessor);
void saveExecutor(Executor *executor, IrqImageAccessor accessor);
void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

// Copies the current state into the executor and calls the supplied function.
extern "C" void doForkExecutor(Executor *executor, void (*functor)(void *), void *context);

void workOnExecutor(Executor *executor);

void scrubStack(FaultImageAccessor accessor, Continuation cont);
void scrubStack(IrqImageAccessor accessor, Continuation cont);
void scrubStack(SyscallImageAccessor accessor, Continuation cont);
void scrubStack(Executor *executor, Continuation cont);

struct OldCpuFeatures {
	static constexpr uint32_t profileIntelSupported = 1;
	static constexpr uint32_t profileAmdSupported = 2;

	bool haveXsave;
	bool haveAvx;
	bool haveZmm;
	bool haveInvariantTsc;
	bool haveTscDeadline;
	bool haveVmx;
	bool haveSvm;
	uint32_t profileFlags;
	size_t xsaveRegionSize;
};

enum class CpuFeatures : uint32_t {
	// EAX=1, ECX
    SSE3 = 0,						// Streaming SIMD Extensions 3
    PCLMULQDQ = 1,					// PCLMULDQ Instruction
    DTES64 = 2,						// 64-Bit Debug Store
    MONITOR = 3,					// MONITOR/MWAIT Instructions
    DS_CPL = 4,						// CPL Qualified Debug Store
    VMX = 5,						// Virtual Machine Extensions
    SMX = 6,						// Safer Mode Extensions
    EST = 7,						// Enhanced Intel SpeedStep® Technology
    TM2 = 8,						// Thermal Monitor 2
    SSSE3 = 9,						// Supplemental Streaming SIMD Extensions 3
    CNXT_ID = 10,					// L1 Context ID
    SDBG = 11,						// Silicon Debug (IA32_DEBUG_INTERFACE MSR)
    FMA = 12,						// Fused Multiply Add
    CX16 = 13,						// CMPXCHG16B Instruction
    XTPR = 14,						// xTPR Update Control
    PDCM = 15,						// Perfmon and Debug Capability (IA32_PERF_CAPABILITIES MSR)
    /* ECX Bit 16 */				// Reserved
    PCID = 17,						// Process Context Identifiers
    DCA = 18,						// Direct Cache Access
    SSE4_1 = 19,					// Streaming SIMD Extensions 4.1
    SSE4_2 = 20,					// Streaming SIMD Extensions 4.2
    X2APIC = 21,					// Extended xAPIC Support
    MOVBE = 22,						// MOVBE Instruction
    POPCNT = 23,					// POPCNT Instruction
    TSC_DEADLINE = 24,				// Time Stamp Counter Deadline
    AES = 25,						// AES Instruction Extensions
    XSAVE = 26,						// XSAVE/XSTOR States
    OSXSAVE = 27,					// OS-Enabled Extended State Management
    AVX = 28,						// Advanced Vector Extensions
    F16C = 29,						// 16-bit floating-point conversion instructions
    RDRAND = 30,					// RDRAND Instruction
    HYPERVISOR = 31,				// Hypervisor present (always zero on physical CPUs)
    // EAX=1, EDX
    FPU = 32,						// Floating-point Unit On-Chip
    VME = 33,						// Virtual Mode Extension
    DE = 34,						// Debugging Extension
    PSE = 35,						// Page Size Extension
    TSC = 36,						// Time Stamp Counter
    MSR = 37,						// Model Specific Registers
    PAE = 38,						// Physical Address Extension
    MCE = 39,						// Machine-Check Exception
    CX8 = 40,						// CMPXCHG8 Instruction
    APIC = 41,						// On-chip APIC Hardware
    /* EDX Bit 10 */				// Reserved
    SEP = 43,						// Fast System Call
    MTRR = 44,						// Memory Type Range Registers
    PGE = 45,						// Page Global Enable
    MCA = 46,						// Machine-Check Architecture
    CMOV = 47,						// Conditional Move Instruction
    PAT = 48,						// Page Attribute Table
    PSE36 = 49,						// 36-bit Page Size Extension
    PSN = 50,						// Processor serial number is present and enabled
    CLFLUSH = 51,					// CLFLUSH Instruction
    /* EDX Bit 20 */				// Reserved
    DS = 53,						// CLFLUSH Instruction
    ACPI = 54,						// CLFLUSH Instruction
    MMX = 55,						// CLFLUSH Instruction
    FXSR = 56,						// CLFLUSH Instruction
    SSE = 57,						// Streaming SIMD Extensions
    SSE2 = 58,						// Streaming SIMD Extensions 2
    SS = 59,						// Self-Snoop
    HTT = 60,						// Multi-Threading
    TM = 61,						// Thermal Monitor
    IA64 = 62,						// IA64 processor emulating x86
    PBE = 63,						// Pending Break Enable
    // EAX=7, EBX
    FSGSBASE = 64,					// Access to base of %fs and %gs
    TSC_ADJUST = 65,				// IA32_TSC_ADJUST MSR
    SGX = 66,						// Software Guard Extensions
    BMI1 = 67,						// Bit Manipulation Instruction Set 1
    HLE = 68,						// TSX Hardware Lock Elision
    AVX2 = 69,						// Advanced Vector Extensions 2
    FDP_EXCPTN_ONLY = 70,			// FDP_EXCPTN_ONLY
    SMEP = 71,						// Supervisor Mode Execution Protection
    BMI2 = 72,						// Bit Manipulation Instruction Set 2
    ERMS = 73,						// Enhanced REP MOVSB/STOSB
    INVPCID = 74,					// INVPCID Instruction
    RTM = 75,						// TSX Restricted Transactional Memory
    PQM = 76,						// Platform Quality of Service Monitoring
    ZERO_FCS_FDS = 77,				// FPU CS and FPU DS deprecated
    MPX = 78,						// Intel MPX (Memory Protection Extensions)
    PQE = 79,						// Platform Quality of Service Enforcement
    AVX512_F = 80,					// AVX-512 Foundation
    AVX512_DQ = 81,					// AVX-512 Doubleword and Quadword Instructions
    RDSEED = 82,					// RDSEED Instruction
    ADX = 83,						// Intel ADX (Multi-Precision Add-Carry Instruction Extensions)
    SMAP = 84,						// Supervisor Mode Access Prevention
    AVX512_IFMA = 85,				// AVX-512 Integer Fused Multiply-Add Instructions
    PCOMMIT = 86,					// PCOMMIT Instruction
    CLFLUSHOPT = 87,				// CLFLUSHOPT Instruction
    CLWB = 88,						// CLWB Instruction
    INTEL_PT = 89,					// Intel Processor Tracing
    AVX512_PF = 90,					// AVX-512 Prefetch Instructions
    AVX512_ER = 91,					// AVX-512 Exponential and Reciprocal Instructions
    AVX512_CD = 92,					// AVX-512 Conflict Detection Instructions
    SHA = 93,						// Intel SHA Extensions
    AVX512_BW = 94,					// AVX-512 Byte and Word Instructions
    AVX512_VL = 95,					// AVX-512 Vector Length Extensions
    // EAX=7, ECX
    PREFETCHWT1 = 96,				// PREFETCHWT1 Instruction
    AVX512_VBMI = 97,				// AVX-512 Vector Bit Manipulation Instructions
    UMIP = 98,						// UMIP
    PKU = 99,						// Memory Protection Keys for User-mode pages
    OSPKE = 100,					// PKU enabled by OS
    WAITPKG = 101,					// Timed pause and user-level monitor/wait
    AVX512_VBMI2 = 102,				// AVX-512 Vector Bit Manipulation Instructions 2
    CET_SS = 103,					// Control Flow Enforcement (CET) Shadow Stack
    GFNI = 104,						// Galois Field Instructions
    VAES = 105,						// Vector AES instruction set (VEX-256/EVEX)
    VPCLMULQDQ = 106,				// CLMUL instruction set (VEX-256/EVEX)
    AVX512_VNNI = 107,				// AVX-512 Vector Neural Network Instructions
    AVX512_BITALG = 108,			// AVX-512 BITALG Instructions
    TME_EN = 109,					// IA32_TME related MSRs are supported
    AVX512_VPOPCNTDQ = 110,			// AVX-512 Vector Population Count Double and Quad-word
    // ECX Bit 15					// Reserved
    INTEL_5_LEVEL_PAGING = 112,		// Intel 5-Level Paging
    RDPID = 113,					// RDPID Instruction
    KL = 114,						// Key Locker
    // ECX Bit 24					// Reserved
    CLDEMOTE = 116,					// Cache Line Demote
    // ECX Bit 26					// Reserved
    MOVDIRI = 118,					// MOVDIRI Instruction
    MOVDIR64B = 119,				// MOVDIR64B Instruction
    ENQCMD = 120,					// ENQCMD Instruction
    SGX_LC = 121,					// SGX Launch Configuration
    PKS = 122,						// Protection Keys for Supervisor-Mode Pages
    // EAX=7, EDX
    // ECX Bit 0-1					// Reserved
    AVX512_4VNNIW = 125,			// AVX-512 4-register Neural Network Instructions
    AVX512_4FMAPS = 126,			// AVX-512 4-register Multiply Accumulation Single precision
    FSRM = 127,						// Fast Short REP MOVSB
    // ECX Bit 5-7					// Reserved
    AVX512_VP2INTERSECT = 131,		// AVX-512 VP2INTERSECT Doubleword and Quadword Instructions
    SRBDS_CTRL = 132,				// Special Register Buffer Data Sampling Mitigations
    MD_CLEAR = 133,					// VERW instruction clears CPU buffers
    RTM_ALWAYS_ABORT = 134,			// All TSX transactions are aborted
    // ECX Bit 12 					// Reserved
    TSX_FORCE_ABORT = 136,			// TSX_FORCE_ABORT MSR
    SERIALIZE = 137,				// Serialize instruction execution
    HYBRID = 138,					// Mixture of CPU types in processor topology
    TSXLDTRK = 139,					// TSX suspend load address tracking
    // ECX Bit 17					// Reserved
    PCONFIG = 141,					// Platform configuration (Memory Encryption Technologies Instructions)
    LBR = 142,						// Architectural Last Branch Records
    CET_IBT = 143,					// Control flow enforcement (CET) indirect branch tracking
    // ECX Bit 21 					// Reserved
    AMX_BF16 = 145,					// Tile computation on bfloat16 numbers
    AVX512_FP16 = 146,				// AVX512-FP16 half-precision floating-point instructions
    AMX_TILE = 147,					// Tile architecture
    AMX_INT8 = 148,					// Tile computation on 8-bit integers
    SPEC_CTRL = 149,				// Speculation Control
    STIBP = 150,					// Single Thread Indirect Branch Predictor
    L1D_FLUSH = 151,				// IA32_FLUSH_CMD MSR
    IA32_ARCH_CAPABILITIES = 152,	// IA32_ARCH_CAPABILITIES MSR
    IA32_CORE_CAPABILITIES = 153,	// IA32_CORE_CAPABILITIES MSR
    SSBD = 154,						// Speculative Store Bypass Disable
    // EAX=80000001h, ECX
    LAHF_LM = 155,					// LAHF/SAHF in long mode
    CMP_LEGACY = 156,				// Hyperthreading not valid
    SVM = 157,						// Secure Virtual Machine
    EXTAPIC = 158,					// Extended APIC Space
    CR8_LEGACY = 159,				// CR8 in 32-bit mode
    ABM = 160,						// Advanced Bit Manipulation
    SSE4A = 161,					// SSE4a
    MISALIGNSSE = 162,				// Misaligned SSE Mode
    _3DNOWPREFETCH = 163,			// PREFETCH and PREFETCHW Instructions
    OSVW = 164,						// OS Visible Workaround
    IBS = 165,						// Instruction Based Sampling
    XOP = 166,						// XOP instruction set
    SKINIT = 167,					// SKINIT/STGI Instructions
    WDT = 168,						// Watchdog timer
    LWP = 169,						// Light Weight Profiling
    FMA4 = 170,						// FMA4 instruction set
    TCE = 171,						// Translation Cache Extension
    NODEID_MSR = 172,				// NodeID MSR
    TBM = 173,						// Trailing Bit Manipulation
    TOPOEXT = 174,					// Topology Extensions
    PERFCTR_CORE = 175,				// Core Performance Counter Extensions
    PERFCTR_NB = 176,				// NB Performance Counter Extensions
    DBX = 177,						// Data Breakpoint Extensions
    PERFTSC = 178,					// Performance TSC
    PCX_L2I = 179,					// L2I Performance Counter Extensions
    // EAX=80000001h, EDX
    SYSCALL = 180,					// SYSCALL/SYSRET Instructions
    MP = 181,						// Multiprocessor Capable
    NX = 182,						// NX bit
    MMXEXT = 183,					// Extended MMX
    FXSR_OPT = 184,					// FXSAVE/FXRSTOR Optimizations
    PDPE1GB = 185,					// Gigabyte Pages
    RDTSCP = 186,					// RDTSCP Instruction
    LM = 187,						// Long Mode
    _3DNOWEXT = 188,				// Extended 3DNow!
    _3DNOW = 189,					// 3DNow!
    // EAX=80000007h, EDX
    CONSTANT_TSC = 190,				// Invariant TSC
    NONSTOP_TSC = 191,				// Invariant TSC
    __End = 255,					// Special marker, should never be set ever
};

extern bool cpuFeaturesKnown;
// This should probably go into the per cpu struct
// As different cores can have different features.
extern OldCpuFeatures globalCpuFeatures;

[[gnu::const]] inline OldCpuFeatures *getGlobalCpuFeatures() {
	assert(cpuFeaturesKnown);
	return &globalCpuFeatures;
}

initgraph::Stage *getCpuFeaturesKnownStage();

// switches the active executor.
// does NOT restore the executor's state.
struct Thread;
void switchExecutor(smarter::borrowed_ptr<Thread> executor);

smarter::borrowed_ptr<Thread> activeExecutor();

// Note: These constants we mirrored in assembly.
// Do not change their values!
inline constexpr unsigned int uarRead = 1;
inline constexpr unsigned int uarWrite = 2;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct UserAccessRegion {
	void *startIp;
	void *endIp;
	void *faultIp;
	unsigned int flags;
};

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer;
	void *syscallStack;
	UserAccessRegion *currentUar;
};

struct PlatformCpuData : public AssemblyCpuData {
	PlatformCpuData();

	int localApicId;

	uint32_t gdt[14 * 2];
	uint32_t idt[256 * 4];

	UniqueKernelStack irqStack;
	UniqueKernelStack dfStack;
	UniqueKernelStack nmiStack;

	common::x86::Tss64 tss;

	PageContext pageContext;
	PageBinding pcidBindings[maxPcidCount];
	GlobalPageBinding globalBinding;

	bool havePcids = false;
	bool haveSmap = false;
	bool haveVirtualization = false;

	LocalApicContext apicContext;

	// TODO: This is not really arch-specific!
	smarter::borrowed_ptr<Thread> activeExecutor;

	uint32_t cpuFeatures;
};

inline PlatformCpuData *getPlatformCpuData() {
	AssemblyCpuData *cpu_data;
	asm volatile ("mov %%gs:0, %0" : "=r"(cpu_data));
	return static_cast<PlatformCpuData *>(cpu_data);
}

inline bool inHigherHalf(uintptr_t address) {
	return address & (static_cast<uintptr_t>(1) << 63);
}

void enableUserAccess();
void disableUserAccess();
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor);

template<typename F, typename... Args>
void runOnStack(F functor, StackBase stack, Args... args) {
	struct Context {
		Context(F functor, Args... args)
		: functor(std::move(functor)), args(std::move(args)...) { }

		F functor;
		frg::tuple<Args...> args;
	};

	Context original(std::move(functor), std::forward<Args>(args)...);
	doRunOnStack([] (void *context, void *previousSp) {
		Context stolen = std::move(*static_cast<Context *>(context));
		frg::apply(std::move(stolen.functor),
				frg::tuple_cat(frg::make_tuple(Continuation{previousSp}), std::move(stolen.args)));
	}, stack.sp, &original);
}

// Calls the given function on the given stack.
void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument);

void setupBootCpuContext();
void initializeThisProcessor();

void bootSecondary(unsigned int apic_id);

template<typename F>
void forkExecutor(F functor, Executor *executor) {
	auto delegate = [] (void *p) {
		auto fp = static_cast<F *>(p);
		(*fp)();
	};

	if(getGlobalCpuFeatures()->haveXsave) {
		common::x86::xsave((uint8_t*)executor->_fxState(), ~0);
	} else {
		asm volatile ("fxsaveq %0" : : "m" (*executor->_fxState()));
	}

	doForkExecutor(executor, delegate, &functor);
}

Error getEntropyFromCpu(void *buffer, size_t size);

void armPreemption(uint64_t nanos);
void disarmPreemption();
bool preemptionIsArmed();

// --------------------------------------------------------
// TSC functionality.
// --------------------------------------------------------

uint64_t getRawTimestampCounter();

inline void pause() {
	asm volatile ("pause");
}

} // namespace thor
