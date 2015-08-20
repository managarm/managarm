
#include <frigg/arch_x86/gdt.hpp>
#include <frigg/arch_x86/idt.hpp>
#include <frigg/arch_x86/tss.hpp>

namespace thor {

// --------------------------------------------------------
// Global runtime functions
// --------------------------------------------------------

typedef uint64_t Word;

typedef uint64_t PhysicalAddr;
typedef uint64_t VirtualAddr;
typedef uint64_t VirtualOffset;

// note: this struct is accessed from assembly.
// do not change the field offsets!
struct ThorRtGeneralState {
	Word rax;		// offset 0x00
	Word rbx;		// offset 0x08
	Word rcx;		// offset 0x10
	Word rdx;		// offset 0x18
	Word rsi;		// offset 0x20
	Word rdi;		// offset 0x28
	Word rbp;		// offset 0x30

	Word r8;		// offset 0x38
	Word r9;		// offset 0x40
	Word r10;		// offset 0x48
	Word r11;		// offset 0x50
	Word r12;		// offset 0x58
	Word r13;		// offset 0x60
	Word r14;		// offset 0x68
	Word r15;		// offset 0x70
	
	Word rsp;		// offset 0x78
	Word rip;		// offset 0x80
	Word rflags;	// offset 0x88
};

struct ThorRtThreadState {
	ThorRtThreadState();

	void activate();

	ThorRtGeneralState generalState;
	frigg::arch_x86::Tss64 threadTss;
};

struct ThorRtCpuSpecific {
	uint32_t gdt[6 * 8];
	uint32_t idt[256 * 16];
	frigg::arch_x86::Tss64 tssTemplate;
};

// note: this struct is accessed from assembly.
// do not change the field offsets!
struct ThorRtKernelGs {
	ThorRtKernelGs();

	void *cpuContext;					// offset 0x00
	ThorRtGeneralState *generalState;	// offset 0x08
	void *syscallStackPtr;				// offset 0x10
	ThorRtCpuSpecific *cpuSpecific;		// offset 0x18
};

void thorRtSetCpuContext(void *context);
void *thorRtGetCpuContext();

void initializeThisProcessor();

void bootSecondary(uint32_t secondary_apic_id);

} // namespace thor

