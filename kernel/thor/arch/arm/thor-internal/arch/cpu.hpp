#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include <frg/tuple.hpp>
#include <frg/vector.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch-generic/cpu-data.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <initgraph.hpp>

#include <thor-internal/arch-generic/asid.hpp>

// NOTE: This header only provides architecture-specific structure and
// inline function definitions. Check arch-generic/cpu.hpp for the
// remaining function prototypes.

namespace thor {

enum class Domain : uint64_t {
	irq = 0,
	fault,
	fiber,
	user,
	idle
};

struct FpRegisters {
	uint64_t v[64]; // V0-V31 are 128 bits
	uint64_t fpcr;
	uint64_t fpsr;
};

struct Frame {
	uint64_t x[31];
	uint64_t sp;
	uint64_t elr;
	uint64_t spsr;
	uint64_t esr;
	uint64_t far;
	Domain domain;
	uint64_t tpidr_el0;

	FpRegisters fp;
};
static_assert(sizeof(Frame) == 832, "Invalid exception frame size");

struct Executor;

struct Continuation {
	void *sp;
};

struct FaultImageAccessor;

struct SyscallImageAccessor {
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

	Word *number() { return &_frame()->x[0]; }
	Word *in0() { return &_frame()->x[1]; }
	Word *in1() { return &_frame()->x[2]; }
	Word *in2() { return &_frame()->x[3]; }
	Word *in3() { return &_frame()->x[4]; }
	Word *in4() { return &_frame()->x[5]; }
	Word *in5() { return &_frame()->x[6]; }
	Word *in6() { return &_frame()->x[7]; }
	Word *in7() { return &_frame()->x[8]; }
	Word *in8() { return &_frame()->x[9]; }

	Word *error() { return &_frame()->x[0]; }
	Word *out0() { return &_frame()->x[1]; }
	Word *out1() { return &_frame()->x[2]; }

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

	Word *ip() { return &_frame()->elr; }
	Word *sp() { return &_frame()->sp; }

	// TODO: this should have a different name
	Word *rflags() { return &_frame()->spsr; }
	Word *code() { return &_frame()->esr ; }

	Word *faultAddr() { return &_frame()->far; }

	bool inKernelDomain() {
		return (_frame()->spsr & 0b1111) != 0b0000;
	}

	bool allowUserPages();

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

	Word *ip() { return &_frame()->elr; }

	// TODO: These are only exposed for debugging.
	// TODO: this should have a different name
	Word *rflags() { return &_frame()->spsr; }

	bool inPreemptibleDomain() {
		return _frame()->domain == Domain::fault
			|| _frame()->domain == Domain::fiber
			|| _frame()->domain == Domain::idle
			|| _frame()->domain == Domain::user;
		return true;
	}

	bool inThreadDomain() {
		assert(inPreemptibleDomain());
		return _frame()->domain == Domain::fault
			|| _frame()->domain == Domain::user;
	}

	bool inManipulableDomain() {
		assert(inThreadDomain());
		return _frame()->domain == Domain::user;
	}

	bool inFiberDomain() {
		assert(inPreemptibleDomain());
		return _frame()->domain == Domain::fiber;
	}

	bool inIdleDomain() {
		assert(inPreemptibleDomain());
		return _frame()->domain == Domain::idle;
	}

	void *frameBase() { return _pointer + sizeof(Frame); }

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

	UserContext();

	UserContext(const UserContext &other) = delete;

	UserContext &operator= (const UserContext &other) = delete;

	// Migrates this UserContext to a different CPU.
	void migrate(CpuData *cpu_data);

	// TODO: This should be private.
	UniqueKernelStack kernelStack;
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

	Executor();

	explicit Executor(UserContext *context, AbiParameters abi);
	explicit Executor(FiberContext *context, AbiParameters abi);

	Executor(const Executor &other) = delete;

	~Executor();

	Executor &operator= (const Executor &other) = delete;

	// FIXME: remove or refactor the rdi / rflags accessors
	// as they are platform specific and need to be abstracted here
	Word *rflags() { return &general()->spsr; }

	Word *ip() { return &general()->elr; }
	Word *sp() { return &general()->sp; }
	Word *cs() { return nullptr; }
	Word *ss() { return nullptr; }

	Word *arg0() { return &general()->x[1]; }
	Word *arg1() { return &general()->x[2]; }
	Word *result0() { return &general()->x[0]; }
	Word *result1() { return &general()->x[1]; }

	Frame *general() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	void *getExceptionStack() {
		return _exceptionStack;
	}

	UserAccessRegion *currentUar() {
		return _uar;
	}

private:
	// Private function only used for the static_assert check.
	//
	// We can't put the static_assert outside because the members are private
	// and we can't put it at the end of the struct body because the type
	// is incomplete at that point.
	static void staticChecks() {
		static_assert(offsetof(Executor, _uar) == THOR_EXECUTOR_UAR);
	}

	char *_pointer;
	void *_exceptionStack;
	UserAccessRegion *_uar{nullptr};
};

size_t getStateSize();

// Determine whether this address belongs to the higher half.
inline constexpr bool inHigherHalf(uintptr_t address) {
	return address & (static_cast<uintptr_t>(1) << 63);
}

void initializeThisProcessor();

void bootSecondary(unsigned int apic_id);

extern "C" void saveFpSimdRegisters(FpRegisters *frame);

// Save the current SIMD register state into the given executor.
inline void saveCurrentSimdState(Executor *executor) {
	saveFpSimdRegisters(&executor->general()->fp);
}

void setupBootCpuContext();

void setupCpuContext(AssemblyCpuData *context);

initgraph::Stage *getBootProcessorReadyStage();

struct CpuData;
void prepareCpuDataFor(CpuData *context, int cpu);

} // namespace thor
