#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/framebuffer.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir-internal/memory-layout.hpp>
#include <initgraph.hpp>

namespace eir {

EirInfo *info_ptr = nullptr;

frg::array<eir::InitialRegion, 32> reservedRegions;
size_t nReservedRegions = 0;

void GlobalInitEngine::preActivate(initgraph::Node *node) {
	infoLogger() << "eir: Running " << node->displayName() << frg::endlog;
}

void GlobalInitEngine::onUnreached() {
	infoLogger() << "eir: initgraph has cycles" << frg::endlog;

	while (1) {
	}
}

constinit GlobalInitEngine globalInitEngine;

initgraph::Stage *getReservedRegionsKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.reserved-regions-known"};
	return &s;
}

initgraph::Stage *getMemoryRegionsKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-regions-known"};
	return &s;
}

initgraph::Stage *getInitrdAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.initrd-available"};
	return &s;
}

initgraph::Stage *getCmdlineAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.cmdline-available"};
	return &s;
}

initgraph::Stage *getKernelLoadableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.kernel-loadable"};
	return &s;
}

initgraph::Stage *getAllocationAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.allocation-available"};
	return &s;
}

initgraph::Stage *getInfoStructAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.info-struct-available"};
	return &s;
}

struct GlobalCtorTest {
	GlobalCtorTest() { infoLogger() << "Hello world from global ctor" << frg::endlog; }
};

static initgraph::Task parseInitrdInfo{
    &globalInitEngine,
    "generic.parse-initrd",
    initgraph::Requires{getInitrdAvailableStage()},
    initgraph::Entails{getReservedRegionsKnownStage()},
    [] {
	    assert(initrd);
	    parseInitrd(initrd);
    }
};

static initgraph::Task earlyProcessorInit{
    &globalInitEngine,
    "generic.early-processor-init",
    initgraph::Requires{getReservedRegionsKnownStage()},
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] { initProcessorEarly(); }
};

static initgraph::Task passFirmwareTables{
    &globalInitEngine,
    "generic.pass-firmware-tables",
    initgraph::Requires{getInfoStructAvailableStage()},
    [] {
	    if (eirRsdpAddr) {
		    info_ptr->acpiRsdp = eirRsdpAddr;
	    }
	    if (eirDtbPtr) {
		    DeviceTree dt{physToVirt<void>(eirDtbPtr)};
		    info_ptr->dtbPtr = eirDtbPtr;
		    info_ptr->dtbSize = dt.size();
	    }
    }
};

static initgraph::Task setupRegionsAndPaging{
    &globalInitEngine,
    "generic.setup-regions-and-page-tables",
    initgraph::Requires{getMemoryRegionsKnownStage()},
    initgraph::Entails{getAllocationAvailableStage()},
    [] {
	    setupRegionStructs();

	    eir::infoLogger() << "Kernel memory regions:" << frg::endlog;
	    for (size_t i = 0; i < numRegions; ++i) {
		    if (regions[i].regionType == RegionType::null)
			    continue;
		    eir::infoLogger() << "    Memory region [" << i << "]."
		                      << " Base: 0x" << frg::hex_fmt{regions[i].address} << ", length: 0x"
		                      << frg::hex_fmt{regions[i].size} << frg::endlog;
		    if (regions[i].regionType == RegionType::allocatable)
			    eir::infoLogger() << "        Buddy tree at 0x"
			                      << frg::hex_fmt{regions[i].buddyTree} << ", overhead: 0x"
			                      << frg::hex_fmt{regions[i].buddyOverhead} << frg::endlog;
	    }

	    initProcessorPaging();
    }
};

static initgraph::Task loadKernelImageTask{
    &globalInitEngine,
    "generic.load-kernel-image",
    initgraph::Requires{getAllocationAvailableStage(), getKernelLoadableStage()},
    [] {
	    // Setup the kernel image.
	    loadKernelImage(reinterpret_cast<void *>(kernel_image.data()));
	    eir::infoLogger() << "eir: Allocated " << (allocatedMemory >> 10)
	                      << " KiB after loading the kernel" << frg::endlog;
    }
};

static initgraph::Task prepareFramebufferForThor{
    &globalInitEngine,
    "generic.prepare-framebuffer-for-thor",
    initgraph::Requires{getAllocationAvailableStage(), getFramebufferAvailableStage()},
    [] {
	    auto *fb = getFramebuffer();
	    if (fb) {
		    // Map the framebuffer.
		    assert(fb->fbAddress & ~static_cast<EirPtr>(pageSize - 1));
		    for (address_t pg = 0; pg < fb->fbPitch * fb->fbHeight; pg += pageSize)
			    mapSingle4kPage(
			        getKernelFrameBuffer() + pg,
			        fb->fbAddress + pg,
			        PageFlags::write,
			        CachingMode::writeCombine
			    );
		    mapKasanShadow(getKernelFrameBuffer(), fb->fbPitch * fb->fbHeight);
		    unpoisonKasanShadow(getKernelFrameBuffer(), fb->fbPitch * fb->fbHeight);

		    info_ptr->frameBuffer = *fb;
		    info_ptr->frameBuffer.fbEarlyWindow = getKernelFrameBuffer();
	    }
    }
};

GlobalCtorTest globalCtorTest;

extern "C" [[noreturn]] void eirMain() {
	infoLogger() << "Entering generic eir setup" << frg::endlog;

	globalInitEngine.run();

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eir::enterKernel();
}

} // namespace eir
