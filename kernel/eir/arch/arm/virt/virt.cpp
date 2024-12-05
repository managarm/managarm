#include <eir-internal/arch/pl011.hpp>
#include <eir-internal/generic.hpp>
#include <frg/manual_box.hpp>

namespace eir {

frg::manual_box<PL011> debugUart;

void debugPrintChar(char c) { debugUart->send(c); }

extern "C" [[noreturn]] void eirVirtMain(uintptr_t deviceTreePtr) {
	debugUart.initialize(0x9000000, 24000000);
	debugUart->init(115200);
	GenericInfo info{
	    .deviceTreePtr = deviceTreePtr,
	    .cmdline = nullptr,
	    .fb{},
	    .debugFlags = eirDebugSerial,
	    .hasFb = false
	};
	eirGenericMain(info);
}

} // namespace eir
