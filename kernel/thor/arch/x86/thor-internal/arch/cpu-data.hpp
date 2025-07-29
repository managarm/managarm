#pragma once

#include <thor-internal/arch/asm.h>
#include <thor-internal/kernel-stack.hpp>
#include <x86/tss.hpp>

namespace thor {

struct Executor;
struct IseqContext;
struct UserAccessRegion;

// Note: This struct is accessed from assembly.
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer;
	Executor *activeExecutor{nullptr};
	void *syscallStack;
	IseqContext *iseqPtr{nullptr};
};

static_assert(offsetof(AssemblyCpuData, selfPointer) == THOR_GS_SELF);
static_assert(offsetof(AssemblyCpuData, activeExecutor) == THOR_GS_EXECUTOR);
static_assert(offsetof(AssemblyCpuData, syscallStack) == THOR_GS_SYSCALL_STACK);
static_assert(offsetof(AssemblyCpuData, iseqPtr) == THOR_GS_ISEQ_PTR);

struct Thread;

struct PlatformCpuData : public AssemblyCpuData {
	PlatformCpuData();

	int localApicId;

	uint32_t gdt[14 * 2];
	uint32_t idt[256 * 4];

	UniqueKernelStack irqStack;
	UniqueKernelStack dfStack;
	UniqueKernelStack nmiStack;

	common::x86::Tss64 tss;

	bool havePcids = false;
	bool haveSmap = false;
	bool haveVirtualization = false;
};

// Get a pointer to this CPU's PlatformCpuData instance.
inline PlatformCpuData *getPlatformCpuData() {
	AssemblyCpuData *cpu_data;
	asm volatile ("mov %%gs:0, %0" : "=r"(cpu_data));
	return static_cast<PlatformCpuData *>(cpu_data);
}

} // namespace thor
