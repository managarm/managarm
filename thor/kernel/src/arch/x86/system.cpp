
#include "generic/kernel.hpp"
#include "hwctrl_service.hpp"

namespace thor {

void initializeTheSystemEarly() {
	initLocalApicOnTheSystem();
	// TODO: managarm crashes on bochs if we do not remap the legacy PIC.
	// we need to debug that and find the cause of this problem.
	setupLegacyPic();
	maskLegacyPic();
}

void initializeTheSystemLater() {
	arch_x86::runHwctrlService();
}

void controlArch(int interface, const void *input, void *output) {
	(void)output;

	switch(interface) {
	case kThorIfSetupHpet: {
		const uint64_t *address_ptr = (const uint64_t *)input;
		setupHpet(*address_ptr);
	} break;
	case kThorIfSetupIoApic: {
		const uint64_t *address_ptr = (const uint64_t *)input;
		setupIoApic(*address_ptr);
	} break;
	case kThorIfBootSecondary: {
		const uint32_t *apic_id = (const uint32_t *)input;
		bootSecondary(*apic_id);
	} break;	
	case kThorIfFinishBoot: {
		// do nothing for now
	} break;
	default:
		assert(!"Illegal interface");
	}
}

} // namespace thor

