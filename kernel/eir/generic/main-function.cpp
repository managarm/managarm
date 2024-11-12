#include <initgraph.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>

namespace eir {

EirFramebuffer *fb = nullptr;
EirInfo *info_ptr = nullptr;

frg::array<eir::InitialRegion, 32> reservedRegions;
size_t nReservedRegions = 0;

frg::string_view cmdline;

void GlobalInitEngine::preActivate(initgraph::Node *node) {
	infoLogger() << "eir: Running " << node->displayName() << frg::endlog;
}

void GlobalInitEngine::onUnreached() {
	infoLogger() << "eir: initgraph has cycles" << frg::endlog;

	while(1) { }
}

constinit GlobalInitEngine globalInitEngine;

initgraph::Stage *getReservedRegionsKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-discovered"};
	return &s;
}

initgraph::Stage *getMemoryRegionsKnownStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.memory-set-up"};
	return &s;
}

initgraph::Stage *getKernelAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.kernel-available"};
	return &s;
}

initgraph::Stage *getAllocationAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.boot-info-buildable"};
	return &s;
}

initgraph::Stage *getInfoStructAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.info-struct-available"};
	return &s;
}

initgraph::Stage *getEirDoneStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.eir-done"};
	return &s;
}

struct GlobalCtorTest {
	GlobalCtorTest() {
		infoLogger() << "Hello world from global ctor" << frg::endlog;
	}
};

static initgraph::Task parseInitrdInfo{&globalInitEngine,
	"generic.parse-initrd",
	initgraph::Entails{getReservedRegionsKnownStage()},
	[] {
		assert(initrd);
		parseInitrd(initrd);
	}
};

static initgraph::Task earlyProcessorInit{&globalInitEngine,
	"generic.early-processor-init",
	initgraph::Requires{getReservedRegionsKnownStage()},
	initgraph::Entails{getMemoryRegionsKnownStage()},
	[] {
		initProcessorEarly();
	}
};

static initgraph::Task setupInfoStruct{&globalInitEngine,
	"generic.setup-thor-info-struct",
	initgraph::Requires{getAllocationAvailableStage()},
	initgraph::Entails{getInfoStructAvailableStage()},
	[] {
		info_ptr = generateInfo(cmdline);
	}
};

static initgraph::Task setupRegionsAndPaging{&globalInitEngine,
	"generic.setup-regions-and-page-tables",
	initgraph::Requires{getMemoryRegionsKnownStage()},
	initgraph::Entails{getAllocationAvailableStage()},
	[] {
		setupRegionStructs();

		eir::infoLogger() << "Kernel memory regions:" << frg::endlog;
		for(size_t i = 0; i < numRegions; ++i) {
			if(regions[i].regionType == RegionType::null)
				continue;
			eir::infoLogger() << "    Memory region [" << i << "]."
					<< " Base: 0x" << frg::hex_fmt{regions[i].address}
					<< ", length: 0x" << frg::hex_fmt{regions[i].size} << frg::endlog;
			if(regions[i].regionType == RegionType::allocatable)
				eir::infoLogger() << "        Buddy tree at 0x" << frg::hex_fmt{regions[i].buddyTree}
						<< ", overhead: 0x" << frg::hex_fmt{regions[i].buddyOverhead}
						<< frg::endlog;
		}

		uint64_t kernel_entry = 0;
		initProcessorPaging(reinterpret_cast<void *>(kernel_image.data()), kernel_entry);
	}
};

static initgraph::Task prepareFramebufferForThor{&globalInitEngine,
	"generic.prepare-framebuffer-for-thor",
	initgraph::Requires{getInfoStructAvailableStage()},
	initgraph::Entails{getEirDoneStage()},
	[] {
		if(fb) {
			// Map the framebuffer.
			assert(fb->fbAddress & ~static_cast<EirPtr>(pageSize - 1));
			for(address_t pg = 0; pg < fb->fbPitch * fb->fbHeight; pg += 0x1000)
				mapSingle4kPage(0xFFFF'FE00'4000'0000 + pg, fb->fbAddress + pg,
						PageFlags::write, CachingMode::writeCombine);
			mapKasanShadow(0xFFFF'FE00'4000'0000, fb->fbPitch * fb->fbHeight);
			unpoisonKasanShadow(0xFFFF'FE00'4000'0000, fb->fbPitch * fb->fbHeight);
			fb->fbEarlyWindow = 0xFFFF'FE00'4000'0000;
		}
	}
};

GlobalCtorTest globalCtorTest;

extern "C" void eirMain() {
	infoLogger() << "Entering generic eir setup" << frg::endlog;

	globalInitEngine.run();

	eir::infoLogger() << "Leaving Eir and entering the real kernel" << frg::endlog;
	eir::enterKernel();
}

} // namespace eir
