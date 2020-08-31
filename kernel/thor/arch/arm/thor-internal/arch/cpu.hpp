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

struct Executor;

struct Continuation {
	void *sp;
};

struct FaultImageAccessor {
	friend void saveExecutor(Executor *executor, FaultImageAccessor accessor);

	// TODO
	Word *ip() { return nullptr; }
	// TODO
	Word *sp() { return nullptr; }
	// TODO
	Word *rflags() { return nullptr; }
	// TODO
	Word *code() { return nullptr; }

	bool inKernelDomain() {
		// TODO
		return true;
	}

	bool allowUserPages();

private:
	// TODO
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct Frame {
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct IrqImageAccessor {
	friend void saveExecutor(Executor *executor, IrqImageAccessor accessor);

	Word *ip() { return nullptr; }

	// TODO: These are only exposed for debugging.
	Word *rflags() { return nullptr; }

	bool inPreemptibleDomain() {
		// TODO
		return true;
	}

	bool inThreadDomain() {
		// TODO
		assert(inPreemptibleDomain());
		return true;
	}

	bool inManipulableDomain() {
		// TODO
		assert(inThreadDomain());
		return true;
	}

	bool inFiberDomain() {
		// TODO
		assert(inPreemptibleDomain());
		return true;
	}

	bool inIdleDomain() {
		// TODO
		assert(inPreemptibleDomain());
		return true;
	}

private:
	// TODO
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct Frame {
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct SyscallImageAccessor {
	friend void saveExecutor(Executor *executor, SyscallImageAccessor accessor);

	// TODO
	Word *number() { return nullptr; }
	// TODO
	Word *in0() { return nullptr; }
	// TODO
	Word *in1() { return nullptr; }
	// TODO
	Word *in2() { return nullptr; }
	// TODO
	Word *in3() { return nullptr; }
	// TODO
	Word *in4() { return nullptr; }
	// TODO
	Word *in5() { return nullptr; }
	// TODO
	Word *in6() { return nullptr; }
	// TODO
	Word *in7() { return nullptr; }
	// TODO
	Word *in8() { return nullptr; }

	// TODO
	Word *error() { return nullptr; }
	// TODO
	Word *out0() { return nullptr; }
	// TODO
	Word *out1() { return nullptr; }

private:
	// TODO
	// this struct is accessed from assembly.
	// do not randomly change its contents.
	struct Frame {
	};

	Frame *_frame() {
		return reinterpret_cast<Frame *>(_pointer);
	}

	char *_pointer;
};

struct NmiImageAccessor {
	// TODO
	void **expectedGs() {
		return nullptr;
	}

	// TODO
	Word *ip() { return nullptr; }
	// TODO
	Word *cs() { return nullptr; }
	// TODO
	Word *rflags() { return nullptr; }

private:
	// TODO
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct Frame {
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

	void *getSyscallStack() {
		return _syscallStack;
	}

	// FIXME: remove or refactor the rdi / rflags accessors
	// as they are platform specific and need to be abstracted here
	// TODO
	Word *rflags() { return nullptr; }

	// TODO
	Word *ip() { return nullptr; }
	// TODO
	Word *sp() { return nullptr; }
	// TODO
	Word *cs() { return nullptr; }
	// TODO
	Word *ss() { return nullptr; }

	// TODO
	Word *arg0() { return nullptr; }
	// TODO
	Word *arg1() { return nullptr; }
	// TODO
	Word *result0() { return nullptr; }
	// TODO
	Word *result1() { return nullptr; }

private:
	// TODO
	// note: this struct is accessed from assembly.
	// do not change the field offsets!
	struct General {
	};

public:
	General *general() {
		return reinterpret_cast<General *>(_pointer);
	}

private:
	char *_pointer;
	void *_syscallStack;
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
	void *syscallStack;
	UserAccessRegion *currentUar;
};

struct PlatformCpuData : public AssemblyCpuData {
	PlatformCpuData();

	uint32_t idt[256 * 4];

	int cpuIndex;

	UniqueKernelStack irqStack;
	UniqueKernelStack nmiStack;
	UniqueKernelStack detachedStack;

	PageContext pageContext;
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

bool intsAreAllowed();
void allowInts();

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

	doForkExecutor(executor, delegate, &functor);
}

Error getEntropyFromCpu(void *buffer, size_t size);

void armPreemption(uint64_t nanos);
void disarmPreemption();
uint64_t getRawTimestampCounter();

void setupBootCpuContext();

} // namespace thor
