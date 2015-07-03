
#include "../../frigg/include/arch_x86/types64.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"

ThorRtThreadState *thorRtUserContext = nullptr;

void *operator new (size_t size, void *pointer) {
	return pointer;
}

void __cxa_pure_virtual() {
	thor::debug::criticalLogger->log("Pure virtual call");
	thorRtHalt();
}

void thorRtInvalidateSpace() {
	asm volatile ("movq %%cr3, %%rax\n\t"
		"movq %%rax, %%cr3" : : : "%rax");
};

