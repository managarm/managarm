#pragma once

#include <eir-internal/generic.hpp>
#include <eir/interface.hpp>
#include <frg/string.hpp>
#include <initgraph.hpp>

namespace eir {

// These fields are set by the boot protocol code.
extern "C" physaddr_t eirDtbPtr;
extern physaddr_t eirRsdpAddr;
extern physaddr_t eirSmbios3Addr;

struct GlobalInitEngine final : initgraph::Engine {
	void preActivate(initgraph::Node *node) override;
	void postActivate(initgraph::Node *node) override;
	void onUnreached() override;
};

extern GlobalInitEngine globalInitEngine;

extern "C" [[noreturn]] void eirMain();

extern "C" void eirRunConstructors();

initgraph::Stage *getInitrdAvailableStage();
initgraph::Stage *getCmdlineAvailableStage();

// Before this stage, all reserved regions must be available.
initgraph::Stage *getReservedRegionsKnownStage();

// Before this stage, all memory regions must be available.
initgraph::Stage *getMemoryRegionsKnownStage();

// After this stage, physical memory can be allocated.
// Ordered after getReservedRegionsKnownStage() and getMemoryRegionsKnownStage().
initgraph::Stage *getAllocationAvailableStage();

// After this stage, memory can be mapped into Thor's address space.
// Ordered after getAllocationAvailableStage().
initgraph::Stage *getKernelMappableStage();

// Before this stage, everything needed to fill out ELF notes but be available.
// After this stage, the kernel image is loaded and the boot information is finalized.
// Ordered after getkernelMappableStage().
initgraph::Stage *getKernelLoadableStage();

extern void *initrd;

extern frg::array<InitialRegion, 32> reservedRegions;
extern size_t nReservedRegions;

} // namespace eir
