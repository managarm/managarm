#pragma once

#include <stddef.h>
#include <stdint.h>
#include <utility>

#include <frg/tuple.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/kernel-stack.hpp>

namespace thor {

enum class Domain : uint64_t {
	irq = 0,
	fault,
	fiber,
	user,
	idle
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

	// FP/SIMD registers
	uint64_t v[64]; // V0-V31 are 128 bits
	uint64_t fpcr;
	uint64_t fpsr;
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

static inline constexpr size_t maxAsid = 256;

struct PlatformCpuData : public AssemblyCpuData {
	PlatformCpuData();

	int cpuIndex;

	UniqueKernelStack irqStack;
	UniqueKernelStack detachedStack;

	PageContext pageContext;
	PageBinding asidBindings[maxAsid];
	GlobalPageBinding globalBinding;

	uint32_t profileFlags = 0;

	// TODO: This is not really arch-specific!
	smarter::borrowed_ptr<Thread> activeExecutor;
};

inline PlatformCpuData *getPlatformCpuData() {
	AssemblyCpuData *cpu_data = nullptr;
	asm ("mrs %0, tpidr_el1" : "=r"(cpu_data));
	return static_cast<PlatformCpuData *>(cpu_data);
}

inline bool inHigherHalf(uintptr_t address) {
	return address & (static_cast<uintptr_t>(1) << 63);
}

void enableUserAccess();
void disableUserAccess();
bool handleUserAccessFault(uintptr_t address, bool write, FaultImageAccessor accessor);

template<typename F, typename... Args>
void runDetached(F functor, Args... args) {
	struct Context {
		Context(F functor, Args... args)
		: functor(std::move(functor)), args(std::move(args)...) { }

		F functor;
		frg::tuple<Args...> args;
	};

	Context original(std::move(functor), std::forward<Args>(args)...);
	doRunDetached([] (void *context, void *sp) {
		Context stolen = std::move(*static_cast<Context *>(context));
		frg::apply(std::move(stolen.functor),
				frg::tuple_cat(frg::make_tuple(Continuation{sp}), std::move(stolen.args)));
	}, &original);
}

// calls the given function on the per-cpu stack
// this allows us to implement a save exit-this-thread function
// that destroys the thread together with its kernel stack
void doRunDetached(void (*function) (void *, void *), void *argument);

void initializeThisProcessor();

void bootSecondary(unsigned int apic_id);

template<typename F>
void forkExecutor(F functor, Executor *executor) {
	auto delegate = [] (void *p) {
		auto fp = static_cast<F *>(p);
		(*fp)();
	};

	//assert(executor->general()->domain == getCpuData()->currentDomain);
	doForkExecutor(executor, delegate, &functor);
}

Error getEntropyFromCpu(void *buffer, size_t size);

void armPreemption(uint64_t nanos);
void disarmPreemption();
uint64_t getRawTimestampCounter();

void setupBootCpuContext();

} // namespace thor
