#include <eir-internal/arch.hpp>
#include <eir-internal/arch/pl011.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <eir/interface.hpp>
#include <eir-internal/memory-layout.hpp>
#include <frg/manual_box.hpp>

namespace eir {

frg::manual_box<PL011> debugUart;

void debugPrintChar(char c) { debugUart->send(c); }

void initPlatform() {
	debugUart.initialize(0x9000000, 24000000);
	debugUart->init(115200);
}

extern "C" [[noreturn]] void eirVirtMain() {
	initPlatform();
	eirRunConstructors();

	GenericInfo info{.cmdline = nullptr, .fb{}, .debugFlags = eirDebugSerial, .hasFb = false};
	eirGenericMain(info);
}

// UART setup for Thor

static initgraph::Task reserveBootUartMmio{
    &globalInitEngine,
    "virt.reserve-boot-uart-mmio",
    initgraph::Entails{getMemoryRegionsKnownStage()},
    [] {
	    reserveEarlyMmio(1);
    }
};

static initgraph::Task setupBootUartMmio{
    &globalInitEngine,
    "virt.setup-boot-uart-mmio",
    initgraph::Requires{getAllocationAvailableStage()},
    initgraph::Entails{getKernelLoadableStage()},
    [] {
	    auto addr = allocateEarlyMmio(1);

	    mapSingle4kPage(addr, 0x9000000, PageFlags::write, CachingMode::mmio);
	    mapKasanShadow(addr, 0x1000);
	    unpoisonKasanShadow(addr, 0x1000);

	    extern BootUartConfig bootUartConfig;
	    bootUartConfig.address = addr;
	    bootUartConfig.type = BootUartType::pl011;
    }
};

} // namespace eir
