#include <eir-internal/arch.hpp>
#include <eir-internal/arch/pl011.hpp>
#include <eir-internal/generic.hpp>
#include <eir-internal/main.hpp>
#include <frg/manual_box.hpp>

namespace eir {

frg::manual_box<PL011> debugUart;

void debugPrintChar(char c) { debugUart->send(c); }

static initgraph::Task prepareSerialForThor{
    &globalInitEngine,
    "virt.prepare-serial-for-thor",
    initgraph::Requires{getEirDoneStage()},
    []() {
	    mapSingle4kPage(0xFFFF'0000'0000'0000, 0x9000000, PageFlags::write, CachingMode::mmio);
	    mapKasanShadow(0xFFFF'0000'0000'0000, 0x1000);
	    unpoisonKasanShadow(0xFFFF'0000'0000'0000, 0x1000);
    }
};

extern "C" [[noreturn]] void eirVirtMain() {
	debugUart.initialize(0x9000000, 24000000);
	debugUart->init(115200);
	eirRunConstructors();

	GenericInfo info{.cmdline = nullptr, .fb{}, .debugFlags = eirDebugSerial, .hasFb = false};
	eirGenericMain(info);
}

} // namespace eir
