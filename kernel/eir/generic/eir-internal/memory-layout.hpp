#pragma once

#include <eir/interface.hpp>
#include <initgraph.hpp>

namespace eir {

// Kernel stack and kernel stack size.
// TODO: This does not need to be global if we move stack allocation into memory-layout.cpp.
extern uint64_t kernelStack;
extern uint64_t kernelStackSize;

// Reserve pages in the early MMIO region.
// Must be called before determineMemoryLayout to determine the size of the region.
void reserveEarlyMmio(uint64_t nPages);

// Allocate pages in the early MMIO region.
// Must be called after determineMemoryLayout.
uint64_t allocateEarlyMmio(uint64_t nPages);

const MemoryLayout &getMemoryLayout();

uint64_t getKernelFrameBuffer();
uint64_t getKernelStackPtr();

// Before this stage: reserveEarlyMmio() must be finished.
// Ordered before getKernelMappableStage().
initgraph::Stage *getMemoryLayoutReservedStage();

} // namespace eir
