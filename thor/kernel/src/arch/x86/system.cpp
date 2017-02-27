
#include "generic/kernel.hpp"
#include "system/acpi/acpi.hpp"
#include "hwctrl_service.hpp"

namespace thor {

namespace pci {
	void pciDiscover();
}

void initializeTheSystemEarly() {
	initLocalApicOnTheSystem();
	// TODO: managarm crashes on bochs if we do not remap the legacy PIC.
	// we need to debug that and find the cause of this problem.
	setupLegacyPic();
	maskLegacyPic();
}

void initializeTheSystemLater() {
	acpi::initialize();
	pci::pciDiscover();
	arch_x86::runHwctrlService();
}

void controlArch(int interface, const void *input, void *output) {
	assert(!"Illegal interface");
}

} // namespace thor

