
#include "../kernel.hpp"

namespace thor {

void initializeTheSystem() {
	initLocalApicOnTheSystem();
	setupLegacyPic();
}

void controlArch(int interface, const void *input, void *output) {
	switch(interface) {
	case kThorIfSetupHpet: {
		const uint64_t *address = (const uint64_t *)input;
		setupHpet(*address);
	} break;
	case kThorIfBootSecondary: {
		const uint32_t *apic_id = (const uint32_t *)input;
		bootSecondary(*apic_id);
	} break;	
	default:
		ASSERT(!"Illegal interface");
	}
}

} // namespace thor

