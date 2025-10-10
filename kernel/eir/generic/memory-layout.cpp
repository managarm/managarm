#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>

namespace eir {

namespace {

MemoryLayout memoryLayout;
// Virtual address of the kernel's boot frame buffer.
uint64_t kernelFrameBuffer;

uint64_t earlyMmioBase;
uint64_t earlyMmioPosition;
uint64_t earlyMmioSize;

} // namespace

void reserveEarlyMmio(uint64_t nPages) {
	assert(!earlyMmioBase);
	earlyMmioSize += nPages * pageSize;
}

uint64_t allocateEarlyMmio(uint64_t nPages) {
	assert(earlyMmioBase);
	assert(earlyMmioPosition < earlyMmioSize);

	uint64_t offset = earlyMmioPosition;
	earlyMmioPosition += nPages * pageSize;
	return earlyMmioBase + offset;
}

uint64_t kernelStack;
uint64_t kernelStackSize;

namespace {

void doDetermineMemoryLayout() {
	auto s = getKernelVirtualBits();
	auto &ml = memoryLayout;

	// First determine the sizes of various regions.
	ml.kernelVirtualSize = 0x8000'0000;
	ml.allocLogSize = 0x1000'0000;
	kernelStackSize = 0x1'0000;

	// Start allocation at the start of the higher half.
	uint64_t nextAddress = -(UINT64_C(1) << (s - 1));

	auto assignLayout = [&](uint64_t size) {
		auto address = nextAddress;
		nextAddress += size;
		return address;
	};

	// The direct physical map takes 1/4 of the entire address space (= 1/2 of the higher half).
	ml.directPhysical = assignLayout(UINT64_C(1) << (s - 2));
	ml.kernelVirtual = assignLayout(ml.kernelVirtualSize);
	ml.allocLog = assignLayout(ml.allocLogSize);
	ml.eirInfo = assignLayout(0x200000);           // 2 MiB should be enough.
	kernelFrameBuffer = assignLayout(0x4000'0000); // 1 GiB.
	kernelStack = assignLayout(kernelStackSize);
	if (earlyMmioSize)
		earlyMmioBase = assignLayout(earlyMmioSize);

	infoLogger() << "eir: Kernel virtual memory layout:" << frg::endlog;
	infoLogger() << "    Direct physical : 0x" << frg::hex_fmt{ml.directPhysical} << frg::endlog;
	infoLogger() << "    Kernel virtual  : 0x" << frg::hex_fmt{ml.kernelVirtual} << frg::endlog;
	infoLogger() << "    Allocation ring : 0x" << frg::hex_fmt{ml.allocLog} << frg::endlog;
	infoLogger() << "    EirInfo         : 0x" << frg::hex_fmt{ml.eirInfo} << frg::endlog;
	infoLogger() << "    Kernel FB       : 0x" << frg::hex_fmt{kernelFrameBuffer} << frg::endlog;
	infoLogger() << "    Kernel stack    : 0x" << frg::hex_fmt{kernelStack} << frg::endlog;
	if (earlyMmioSize)
		infoLogger() << "    Early MMIO      : 0x" << frg::hex_fmt{earlyMmioBase} << frg::endlog;
	else
		infoLogger() << "    Early MMIO      : (not assigned)" << frg::endlog;
}

} // namespace

const MemoryLayout &getMemoryLayout() { return memoryLayout; }

uint64_t getKernelFrameBuffer() { return kernelFrameBuffer; }
uint64_t getKernelStackPtr() { return kernelStack + kernelStackSize; }

namespace {

initgraph::Task determineMemoryLayout{
    &globalInitEngine,
    "generic.determine-memory-layout",
    initgraph::Requires{getMemoryLayoutReservedStage()},
    initgraph::Entails{getKernelMappableStage()},
    [] { doDetermineMemoryLayout(); }
};

initgraph::Task setupKernelStackHeap{
    &globalInitEngine,
    "generic.setup-kernel-stack-heap",
    initgraph::Requires{getKernelMappableStage()},
    [] {
	    // Setup the kernel stack.
	    for (address_t page = 0; page < kernelStackSize; page += pageSize)
		    mapSingle4kPage(kernelStack + page, allocPage(), PageFlags::write);
	    mapKasanShadow(kernelStack, kernelStackSize);
	    unpoisonKasanShadow(kernelStack, kernelStackSize);

	    mapKasanShadow(memoryLayout.kernelVirtual, memoryLayout.kernelVirtualSize);
    }
};

} // namespace

initgraph::Stage *getMemoryLayoutReservedStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-layout-reserved-stage"};
	return &s;
}

} // namespace eir
