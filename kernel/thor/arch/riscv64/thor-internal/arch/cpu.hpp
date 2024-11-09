#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include <frg/tuple.hpp>
#include <frg/vector.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <initgraph.hpp>

namespace thor {

enum class Domain : uint64_t {
	irq = 0,
	fault,
	fiber,
	user,
	idle
};

struct FpRegisters {
	// F - single precision floating point - 32 bits / number
	// D - double precision floating point - 64 bits / number
	// Q - quadruple precision floating point - 128 bits / number
	// Since there are always 32 registers, 64 uint64_t's are required
	// in order to support all possible hardware floating point
	// configurations.
	uint64_t v[64];
	uint64_t fpcr;
	uint64_t fpsr;
};

struct Frame {
	uint64_t x[30]; // X0 is constant zero, no need to save it
	uint64_t ip;
	Domain domain;

	FpRegisters fp;
};
// TODO: add static assert

struct Executor;

struct Continuation {
	void *sp;
};

struct FaultImageAccessor;

struct SyscallImageAccessor {
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

	// The "- 1" is since we do not save x0
	// this makes the first number the register ID
	// We begin from A0
	// in7 and in8 are actually S2 and S3, since (according to
	// the calling convention) not enough argument registers
	Word *number() { return &_frame()->x[10 - 1]; }
	Word *in0() { return &_frame()->x[11 - 1]; }
	Word *in1() { return &_frame()->x[12 - 1]; }
	Word *in2() { return &_frame()->x[13 - 1]; }
	Word *in3() { return &_frame()->x[14 - 1]; }
	Word *in4() { return &_frame()->x[15 - 1]; }
	Word *in5() { return &_frame()->x[16 - 1]; }
	Word *in6() { return &_frame()->x[17 - 1]; }
	Word *in7() { return &_frame()->x[18 - 1]; }
	Word *in8() { return &_frame()->x[19 - 1]; }

	Word *error() { return &_frame()->x[10 - 1]; }	
	Word *out0() { return &_frame()->x[11 - 1]; }
	Word *out1() { return &_frame()->x[12 - 1]; }

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	friend struct FaultImageAccessor;

	SyscallImageAccessor(char *ptr)
	: _pointer{ptr} { }

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct FaultImageAccessor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);

	// TODO: this

	Word *ip() { assert(!"Not implemented"); }
	Word *sp() { assert(!"Not implemented"); }

	// We have several flags registers, not sure how to best do this	
	Word *rflags()  { assert(!"Not implemented"); }
	Word *code() { assert(!"Not implemented"); }

	bool inKernelDomain() { assert(!"Not implemented"); }

	bool allowUserPages() { assert(!"Not implemented"); }

	operator SyscallImageAccessor () {
		return SyscallImageAccessor{_pointer};
	}

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct IrqImageAccessor {
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);

	Word *ip() { assert(!"Not implemented"); }
	Word *rflags() { assert(!"Not implemented"); }

	bool inPreemptibleDomain() { assert(!"Not implemented"); }

	bool inThreadDomain() { assert(!"Not implemented"); }

	bool inManipulableDomain() { assert(!"Not implemented"); }

	bool inFiberDomain() { assert(!"Not implemented"); }

	bool inIdleDomain() { assert(!"Not implemented"); }

	void *frameBase() { assert(!"Not implemented"); }

private:
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

	UserContext() { assert(!"Not implemented"); }

	UserContext(const UserContext &other) = delete;

	UserContext &operator= (const UserContext &other) = delete;

	// Migrates this UserContext to a different CPU.
	void migrate(CpuData *cpu_data) { assert(!"Not implemented"); }

	// TODO: This should be private.
	UniqueKernelStack kernelStack;
};

struct FiberContext {
	FiberContext(UniqueKernelStack stack) { assert(!"Not implemented"); }

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

	Executor() { assert(!"Not implemented"); }

	explicit Executor(UserContext *context, AbiParameters abi) { assert(!"Not implemented"); }
	explicit Executor(FiberContext *context, AbiParameters abi) { assert(!"Not implemented"); }

	Executor(const Executor &other) = delete;

	~Executor() { assert(!"Not implemented"); }

	Executor &operator= (const Executor &other) = delete;

	// FIXME: remove or refactor the rdi / rflags accessors
	// as they are platform specific and need to be abstracted here
	Word *rflags() { assert(!"Not implemented"); }

	Word *ip() { assert(!"Not implemented"); }
	Word *sp() { assert(!"Not implemented"); }
	Word *cs() { return nullptr; }
	Word *ss() { return nullptr; }

	Word *arg0() { assert(!"Not implemented"); }
	Word *arg1() { assert(!"Not implemented"); }
	Word *result0() { assert(!"Not implemented"); }
	Word *result1() { assert(!"Not implemented"); }

	Frame *general() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	void *getExceptionStack() {
		return _exceptionStack;
	}

private:
	char *_pointer;
	void *_exceptionStack;
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

size_t getStateSize();

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
	uint64_t currentDomain;
	void *exceptionStackPtr;
	void *irqStackPtr;
	UserAccessRegion *currentUar;
};


// TODO: hardware dependent
// To determine the amount of implemented ASID bits,
// we need to write a 1 to every bit in the ASID field
// in SATP (aka the ASIDLEN), where the max ASIDLEN is
// 16 -> 2**16 -> 65535
static inline constexpr size_t maxAsid = 65535;

struct PlatformCpuData : public AssemblyCpuData {
	PlatformCpuData() { assert(!"Not implemented"); }

	UniqueKernelStack irqStack;

	PageContext pageContext;
	PageBinding asidBindings[maxAsid];
	GlobalPageBinding globalBinding;

	uint32_t profileFlags = 0;

	bool preemptionIsArmed = false;

	// TODO: This is not really arch-specific!
	smarter::borrowed_ptr<Thread> activeExecutor;
};

inline PlatformCpuData *getPlatformCpuData() { assert(!"Not implemented"); }

inline bool inHigherHalf(uintptr_t address) {
	return address & (static_cast<uintptr_t>(1) << 63);
}

void enableUserAccess();
void disableUserAccess();
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor);

template<typename F, typename... Args>
void runOnStack(F functor, StackBase stack, Args... args) { assert(!"Not implemented"); }

// Calls the given function on the given stack.
void doRunOnStack(void (*function) (void *, void *), void *sp, void *argument);

void initializeThisProcessor();

void bootSecondary(unsigned int apic_id);

template<typename F>
void forkExecutor(F functor, Executor *executor) { assert(!"Not implemented"); }

Error getEntropyFromCpu(void *buffer, size_t size);

void armPreemption(uint64_t nanos);
void disarmPreemption();
uint64_t getRawTimestampCounter();

void setupBootCpuContext();

void setupCpuContext(AssemblyCpuData *context);

initgraph::Stage *getBootProcessorReadyStage();

bool preemptionIsArmed();

}