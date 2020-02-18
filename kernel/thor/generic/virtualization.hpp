#pragma once
#include "../../../hel/include/hel.h"
#include <generic/error.hpp>

namespace thor {
	struct GuestState {
		uint64_t rax;
		uint64_t rbx;
		uint64_t rcx;
		uint64_t rdx;
		uint64_t rsi;
		uint64_t rdi;
		uint64_t rbp;
		uint64_t r8;
		uint64_t r9;
		uint64_t r10;
		uint64_t r11;
		uint64_t r12;
		uint64_t r13;
		uint64_t r14;
		uint64_t r15;
	} __attribute__((packed));

	struct VirtualizedCpu {
			virtual HelVmexitReason run();
			virtual void storeRegs(const HelX86VirtualizationRegs *regs);
			virtual void loadRegs(HelX86VirtualizationRegs *res);
	};

	struct VirtualizedPageSpace {
		virtual Error store(uintptr_t guestAddress, size_t len, const void* buffer);
		virtual Error load(uintptr_t guestAddress, size_t len, void* buffer);

		virtual Error map(uint64_t guestAddress, int flags);
	};
}
