#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include <frg/tuple.hpp>
#include <frg/vector.hpp>
#include <initgraph.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-stack.hpp>
#include <thor-internal/types.hpp>

#include <thor-internal/arch-generic/asid.hpp>

namespace thor {

enum class Domain : uint64_t { irq = 0, fault, fiber, user, idle };

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
	uint64_t xs[31]; // X0 is constant zero, no need to save it.
	uint64_t ip;
	uint64_t umode;

	constexpr uint64_t &x(unsigned int n) {
		assert(n > 0 && n <= 31);
		return xs[n - 1];
	}

	constexpr uint64_t &a(unsigned int n) {
		assert(n <= 7);
		return x(10 + n);
	}
	constexpr uint64_t &ra() { return x(1); }
	constexpr uint64_t &sp() { return x(2); }
};
static_assert(offsetof(Frame, ip) == 0xF8);
static_assert(sizeof(Frame) == 0x108);

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
	Word *number() { return &_frame()->xs[10 - 1]; }
	Word *in0() { return &_frame()->xs[11 - 1]; }
	Word *in1() { return &_frame()->xs[12 - 1]; }
	Word *in2() { return &_frame()->xs[13 - 1]; }
	Word *in3() { return &_frame()->xs[14 - 1]; }
	Word *in4() { return &_frame()->xs[15 - 1]; }
	Word *in5() { return &_frame()->xs[16 - 1]; }
	Word *in6() { return &_frame()->xs[17 - 1]; }
	Word *in7() { return &_frame()->xs[18 - 1]; }
	Word *in8() { return &_frame()->xs[19 - 1]; }

	Word *error() { return &_frame()->xs[10 - 1]; }
	Word *out0() { return &_frame()->xs[11 - 1]; }
	Word *out1() { return &_frame()->xs[12 - 1]; }

	void *frameBase() { return _pointer + sizeof(Frame); }

private:
	friend struct FaultImageAccessor;

	SyscallImageAccessor(char *ptr) : _pointer{ptr} {}

	Frame *_frame() { return reinterpret_cast<Frame *>(_pointer); }

	char *_pointer;
};

struct FaultImageAccessor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);

	FaultImageAccessor(Frame *frame) : _pointer{frame} {}

	Word *ip() { return &_frame()->ip; }
	Word *sp() { unimplementedOnRiscv(); }

	// TODO: There are several flag registers on RISC-V.
	Word *rflags() { unimplementedOnRiscv(); }
	Word *code() { unimplementedOnRiscv(); }

	bool inKernelDomain() { return !_frame()->umode; }

	// TODO: Implement the SUM bit in sstatus.
	bool allowUserPages() { return false; }

	void *frameBase() { return reinterpret_cast<char *>(_pointer) + sizeof(Frame); }

private:
	Frame *_frame() { return reinterpret_cast<Frame *>(_pointer); }

	void *_pointer;
};

struct IrqImageAccessor {
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);

	Word *ip() { unimplementedOnRiscv(); }
	Word *rflags() { unimplementedOnRiscv(); }

	bool inPreemptibleDomain() { unimplementedOnRiscv(); }

	bool inThreadDomain() { unimplementedOnRiscv(); }

	bool inManipulableDomain() { unimplementedOnRiscv(); }

	bool inFiberDomain() { unimplementedOnRiscv(); }

	bool inIdleDomain() { unimplementedOnRiscv(); }

	void *frameBase() { unimplementedOnRiscv(); }

private:
	Frame *_frame() { return reinterpret_cast<Frame *>(_pointer); }

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

	UserContext &operator=(const UserContext &other) = delete;

	// Migrates this UserContext to a different CPU.
	void migrate(CpuData *cpuData);

	// TODO: This should be private.
	UniqueKernelStack kernelStack;
};

struct FiberContext {
	FiberContext(UniqueKernelStack s) : stack{std::move(s)} {}

	FiberContext(const FiberContext &other) = delete;

	FiberContext &operator=(const FiberContext &other) = delete;

	// TODO: This should be private.
	UniqueKernelStack stack;
};

struct Executor;

// Restores the current executor from its saved image.
// This function does the heavy lifting during task switch.
// Note: due to the attribute, this must be declared before the friend declaration below.
[[noreturn]] void restoreExecutor(Executor *executor);

struct Executor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);
	friend void workOnExecutor(Executor *executor);
	friend void restoreExecutor(Executor *executor);

	// TODO: Support FPU / SIMD state.
	static size_t determineSize() { return sizeof(Frame); }

	Executor() { unimplementedOnRiscv(); }

	explicit Executor(UserContext *context, AbiParameters abi);
	explicit Executor(FiberContext *context, AbiParameters abi);

	Executor(const Executor &other) = delete;

	~Executor() { unimplementedOnRiscv(); }

	Executor &operator=(const Executor &other) = delete;

	Word *ip() { unimplementedOnRiscv(); }
	Word *sp() { return &general()->sp(); }

	Word *arg0() { unimplementedOnRiscv(); }
	Word *arg1() { unimplementedOnRiscv(); }
	Word *result0() { unimplementedOnRiscv(); }
	Word *result1() { unimplementedOnRiscv(); }

	Frame *general() { return reinterpret_cast<Frame *>(_pointer); }

	void *getExceptionStack() { return _exceptionStack; }

private:
	char *_pointer{nullptr};
	void *_exceptionStack{nullptr};
};

size_t getStateSize();

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

struct IseqContext;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer; //  0x0
	uint64_t currentDomain;       //  0x8
	void *exceptionStackPtr;      // 0x10
	void *irqStackPtr;            // 0x18
	uint64_t scratchSp;           // 0x20
	UserAccessRegion *currentUar;
	IseqContext *iseqPtr;
};

struct Thread;

struct PlatformCpuData : public AssemblyCpuData {
	uint64_t hartId{~UINT64_C(0)};

	UniqueKernelStack irqStack;

	frg::manual_box<AsidCpuData> asidData;

	uint32_t profileFlags = 0;

	bool preemptionIsArmed = false;

	// TODO: This is not really arch-specific!
	smarter::borrowed_ptr<Thread> activeExecutor;
};

// Get a pointer to this CPU's PlatformCpuData instance.
inline PlatformCpuData *getPlatformCpuData() {
	AssemblyCpuData *result;
	asm volatile("mv %0, tp" : "=r"(result));
	return static_cast<PlatformCpuData *>(result);
}

// Determine whether this address belongs to the higher half.
inline constexpr bool inHigherHalf(uintptr_t address) {
	return address & (static_cast<uintptr_t>(1) << 63);
}

void bootSecondary(unsigned int apic_id);

size_t getCpuCount();

// TODO: This is not a no-op if we enable the FPU.
inline void saveCurrentSimdState(Executor *executor) {}

void setupBootCpuContext();

initgraph::Stage *getBootProcessorReadyStage();

} // namespace thor
