#pragma once

#include <eir/interface.hpp>

namespace eir {

// Kernel stack and kernel stack size.
// TODO: This does not need to be global if we move stack allocation into memory-layout.cpp.
extern uint64_t kernelStack;
extern uint64_t kernelStackSize;

// Compute the members of memoryLayout.
// Also determine the kernel stack pointer.
void determineMemoryLayout();

const MemoryLayout &getMemoryLayout();

uint64_t getKernelFrameBuffer();
uint64_t getKernelStackPtr();

} // namespace eir
