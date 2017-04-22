
#include "generic/kernel.hpp"
#include "system/acpi/acpi.hpp"

namespace thor {

void initializeTheSystemEarly() {
	initLocalApicOnTheSystem();
	// TODO: managarm crashes on bochs if we do not remap the legacy PIC.
	// we need to debug that and find the cause of this problem.
	setupLegacyPic();
	maskLegacyPic();
}

void initializeBasicSystem() {
	acpi::initializeBasicSystem();
}

void initializeExtendedSystem() {
	acpi::initializeExtendedSystem();
}

} // namespace thor

