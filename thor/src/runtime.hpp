
#include "../../frigg/include/arch_x86/gdt.hpp"
#include "../../frigg/include/arch_x86/idt.hpp"
#include "../../frigg/include/arch_x86/tss.hpp"

// --------------------------------------------------------
// Global runtime functions
// --------------------------------------------------------

typedef uint64_t Word;

typedef uint64_t PhysicalAddr;
typedef uint64_t VirtualAddr;
typedef uint64_t VirtualOffset;

extern "C" void thorRtHalt();

struct ThorRtThreadState {
	Word rax;		// offset 0
	Word rbx;		// offset 0x8
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
	Word rflags;		// offset 0x88
};

extern ThorRtThreadState *thorRtUserContext;

void thorRtInitializeProcessor();
void thorRtSetupIrqs();
void thorRtAcknowledgeIrq(int irq);

extern "C" void thorRtInvalidatePage(void *pointer);
extern "C" void thorRtInvalidateSpace();

extern "C" void thorRtFullReturn();
extern "C" void thorRtReturnSyscall1(Word out0);
extern "C" void thorRtReturnSyscall2(Word out0, Word out1);
extern "C" void thorRtReturnSyscall3(Word out0, Word out1, Word out2);

void thorRtEnableTss(frigg::arch_x86::Tss64 *tss_pointer);

// --------------------------------------------------------
// Internal runtime functions
// --------------------------------------------------------

extern "C" void thorRtLoadCs(uint16_t selector);

extern "C" void thorRtIsrDoubleFault();
extern "C" void thorRtIsrPageFault();
extern "C" void thorRtIsrIrq0();
extern "C" void thorRtIsrIrq1();
extern "C" void thorRtIsrIrq2();
extern "C" void thorRtIsrIrq3();
extern "C" void thorRtIsrIrq4();
extern "C" void thorRtIsrIrq5();
extern "C" void thorRtIsrIrq6();
extern "C" void thorRtIsrIrq7();
extern "C" void thorRtIsrIrq8();
extern "C" void thorRtIsrIrq9();
extern "C" void thorRtIsrIrq10();
extern "C" void thorRtIsrIrq11();
extern "C" void thorRtIsrIrq12();
extern "C" void thorRtIsrIrq13();
extern "C" void thorRtIsrIrq14();
extern "C" void thorRtIsrIrq15();
extern "C" void thorRtIsrSyscall();

template<typename T>
class LazyInitializer {
public:
	template<typename... Args>
	void initialize(Args&&... args) {
		new(p_object) T(frigg::traits::forward<Args>(args)...);
	}

	T *get() {
		return (T *)p_object;
	}
	T *operator-> () {
		return get();
	}
	T &operator* () {
		return *get();
	}

private:
	char p_object[sizeof(T)];
};

