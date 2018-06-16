#ifndef THOR_ARCH_X86_CPU_HPP
#define THOR_ARCH_X86_CPU_HPP

#include <stddef.h>
#include <stdint.h>
#include <utility>
#include <frigg/algorithm.hpp>
#include <frigg/arch_x86/gdt.hpp>
#include <frigg/arch_x86/idt.hpp>
#include <frigg/arch_x86/tss.hpp>
#include <frigg/smart_ptr.hpp>
#include <frigg/tuple.hpp>
#include "../../generic/types.hpp"
#include "ints.hpp"
#include "paging.hpp"
#include "pic.hpp"

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

struct UniqueKernelStack {
	static constexpr size_t kSize = 0x2000;

	static UniqueKernelStack make();

	friend void swap(UniqueKernelStack &a, UniqueKernelStack &b) {
		frigg::swap(a._base, b._base);
	}

	UniqueKernelStack()
	: _base(nullptr) { }

	UniqueKernelStack(const UniqueKernelStack &other) = delete;

	UniqueKernelStack(UniqueKernelStack &&other)
	: UniqueKernelStack() {
		swap(*this, other);
	}

	~UniqueKernelStack();

	UniqueKernelStack &operator= (UniqueKernelStack other) {
		swap(*this, other);
		return *this;
	}

	void *base() {
		return _base;
	}
	
	template<typename T, typename... Args>
	T *embed(Args &&... args) {
		// TODO: Do not use a magic number as stack alignment here.
		_base -= (sizeof(T) + 15) & ~size_t{15};
		return new (_base) T{frigg::forward<Args>(args)...};
	}

	bool contains(void *sp) {
		return uintptr_t(sp) >= uintptr_t(_base) + kSize
				&& uintptr_t(sp) <= uintptr_t(_base);
	}

private:
	explicit UniqueKernelStack(char *base)
	: _base(base) { }

	char *_base;
};

struct Executor;

struct FaultImageAccessor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);

	Word *ip() { return &_frame()->rip; }

	Word *cs() { return &_frame()->cs; }
	Word *rflags() { return &_frame()->rflags; }
	Word *code() { return &_frame()->code; }
	
	bool inKernelDomain() {
		if(*cs() == kSelSystemIdleCode
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

	bool inFiberDomain() {
		assert(inPreemptibleDomain());
		return *cs() == kSelSystemFiberCode;
	}

	bool inIdleDomain() {
		assert(inPreemptibleDomain());
		return *cs() == kSelSystemIdleCode;
	}

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
CpuData *getCpuData();
CpuData *getCpuData(size_t k);
int getCpuCount();

struct AbiParameters {
	uintptr_t ip;
	uintptr_t sp;
	uintptr_t argument;
};

struct UserContext {
	UserContext();

	UserContext(const UserContext &other) = delete;

	UserContext &operator= (const UserContext &other) = delete;

	void enableIoPort(uintptr_t port);

	// Migrates this UserContext to a different CPU.
	void migrate(CpuData *cpu_data);

	// TODO: This should be private.
	UniqueKernelStack kernelStack;
	frigg::arch_x86::Tss64 tss;
};

struct FiberContext {
	FiberContext(UniqueKernelStack stack);

	FiberContext(const FiberContext &other) = delete;

	FiberContext &operator= (const FiberContext &other) = delete;

	// TODO: This should be private.
	UniqueKernelStack stack;
};

struct Executor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);
	friend void restoreExecutor(Executor *executor);

	static size_t determineSize();

	Executor();
	
	explicit Executor(UserContext *context, AbiParameters abi);
	explicit Executor(FiberContext *context, AbiParameters abi);

	Executor(const Executor &other) = delete;

	~Executor();

	Executor &operator= (const Executor &other) = delete;

	// FIXME: remove or refactor the rdi / rflags accessors
	// as they are platform specific and need to be abstracted here
	Word *rflags() { return &general()->rflags; }

	Word *ip() { return &general()->rip; }
	Word *sp() { return &general()->rsp; }

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

private:
	
	FxState *_fxState() {
		return reinterpret_cast<FxState *>(_pointer + sizeof(General));
	}

	char *_pointer;
	void *_syscallStack;
	frigg::arch_x86::Tss64 *_tss;
};

void saveExecutor(Executor *executor, FaultImageAccessor accessor);
void saveExecutor(Executor *executor, IrqImageAccessor accessor);
void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

template<typename F>
void forkExecutor(F functor, Executor *executor) {
	auto delegate = [] (void *p) {
		auto fp = static_cast<F *>(p);
		(*fp)();
	};
	doForkExecutor(executor, delegate, &functor);
}

// Copies the current state into the executor and calls the supplied function.
extern "C" void doForkExecutor(Executor *executor, void (*functor)(void *), void *context);

// restores the current executor from its saved image.
// this is functions does the heavy lifting during task switch.
[[ noreturn ]] void restoreExecutor(Executor *executor);

size_t getStateSize();

// switches the active executor.
// does NOT restore the executor's state.
struct Thread;
void switchExecutor(frigg::UnsafePtr<Thread> executor);

frigg::UnsafePtr<Thread> activeExecutor();

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer;
	void *syscallStack;
};

struct PlatformCpuData : public AssemblyCpuData {
	PlatformCpuData();

	int localApicId;

	uint32_t gdt[14 * 2];
	uint32_t idt[256 * 4];

	UniqueKernelStack irqStack;
	UniqueKernelStack nmiStack;
	UniqueKernelStack detachedStack;
	
	frigg::arch_x86::Tss64 tss;

	PageContext pageContext;
	PageBinding pcidBindings[maxPcidCount];

	bool haveSmap;
	bool havePcids;

	LocalApicContext apicContext;
	
	// TODO: This is not really arch-specific!
	frigg::UnsafePtr<Thread> activeExecutor;
};

void enableUserAccess();
void disableUserAccess();

bool intsAreAllowed();
void allowInts();

template<typename F, typename... Args>
void runDetached(F functor, Args... args) {
	struct Context {
		Context(F functor, Args... args)
		: functor(frigg::move(functor)), args(frigg::move(args)...) { }

		F functor;
		frigg::Tuple<Args...> args;
	};

	Context original(frigg::move(functor), frigg::forward<Args>(args)...);
	doRunDetached([] (void *context) {
		Context stolen = frigg::move(*static_cast<Context *>(context));
		frigg::applyToFunctor(frigg::move(stolen.functor), frigg::move(stolen.args));
	}, &original);
}

// calls the given function on the per-cpu stack
// this allows us to implement a save exit-this-thread function
// that destroys the thread together with its kernel stack
void doRunDetached(void (*function) (void *), void *argument);

void earlyInitializeBootProcessor();
void initializeBootProcessor();
void initializeThisProcessor();

void bootSecondary(unsigned int apic_id);

} // namespace thor

#endif // THOR_ARCH_X86_CPU_HPP
