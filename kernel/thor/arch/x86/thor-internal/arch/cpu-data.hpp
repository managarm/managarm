#pragma once

#include <thor-internal/kernel-stack.hpp>
#include <x86/tss.hpp>

namespace thor {

struct IseqContext;
struct UserAccessRegion;

// Note: This struct is accessed from assembly.
// Do not change the field offsets!
struct AssemblyCpuData {
	AssemblyCpuData *selfPointer;
	void *syscallStack;
	UserAccessRegion *currentUar;
	IseqContext *iseqPtr{nullptr};
};

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
